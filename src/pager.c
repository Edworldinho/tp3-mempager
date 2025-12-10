#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE

#include <stdint.h>      /* Para intptr_t, uintptr_t */
#include <sys/mman.h>   /* Para PROT_READ, PROT_WRITE, PROT_NONE */
#include <unistd.h>     /* Para sysconf, _SC_PAGESIZE, pid_t */

#include "pager.h"
#include "mmu.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

/* ==================== ESTRUTURAS DE DADOS ==================== */

/* Estado de uma página virtual */
typedef enum {
    PAGE_UNINITIALIZED,  /* Nunca acessada, sem quadro nem dados */
    PAGE_ON_DISK,        /* Em disco, não na memória */
    PAGE_IN_MEMORY       /* Na memória RAM */
} page_state_t;

/* Entrada na tabela de páginas de um processo */
typedef struct {
    page_state_t state;
    int frame;          /* Quadro físico (se em memória) */
    int disk_block;     /* Bloco de disco (sempre alocado após extend) */
    int prot;           /* Permissões atuais (PROT_NONE/READ/WRITE) */
    int referenced;     /* Bit de referência para segunda chance */
    int dirty;          /* Página modificada? */
    int initialized;    /* Já foi zerada? */
    int saved_on_disk;  /* 1 se o bloco de disco tem dados válidos */
} page_entry_t;

/* Tabela de páginas de um processo */
typedef struct process_table {
    pid_t pid;
    page_entry_t *pages;        /* Array de páginas virtuais */
    int page_count;             /* Número de páginas alocadas */
    struct process_table *next; /* Para lista encadeada */
} process_table_t;

/* Entrada na tabela de quadros físicos */
typedef struct {
    int free;           /* 1 se livre, 0 se ocupado */
    pid_t pid;          /* Processo dono (se ocupado) */
    int page_index;     /* Índice da página no processo (se ocupado) */
    int referenced;     /* Bit de referência (para segunda chance) */
} frame_entry_t;

/* Estrutura global do paginador */
static struct {
    int nframes;                /* Número de quadros físicos */
    int nblocks;                /* Número de blocos de disco */

    frame_entry_t *frames;      /* Tabela de quadros físicos */
    int *free_blocks;           /* Lista de blocos livres (1=livre) */
    int free_block_count;       /* Contador de blocos livres */

    process_table_t *processes; /* Lista de tabelas de processos */

    int clock_hand;             /* Ponteiro do relógio (segunda chance) */
    pthread_mutex_t mutex;      /* Mutex para sincronização */
} pager;

/* ==================== FUNÇÕES AUXILIARES ==================== */

/* Encontra a tabela de páginas de um processo */
static process_table_t* find_process_table(pid_t pid) {
    process_table_t *proc = pager.processes;
    while (proc && proc->pid != pid) {
        proc = proc->next;
    }
    return proc;
}

/* Cria uma nova tabela de páginas para um processo */
static process_table_t* create_process_table(pid_t pid) {
    process_table_t *proc = malloc(sizeof(process_table_t));
    if (!proc) return NULL;

    proc->pid = pid;
    proc->pages = NULL;
    proc->page_count = 0;
    proc->next = pager.processes;
    pager.processes = proc;

    return proc;
}

/* Libera a tabela de páginas de um processo */
static void destroy_process_table(process_table_t *proc) {
    if (!proc) return;

    process_table_t **prev = &pager.processes;
    while (*prev && *prev != proc) {
        prev = &(*prev)->next;
    }
    if (*prev) *prev = proc->next;

    /* Libera páginas */
    if (proc->pages) free(proc->pages);
    free(proc);
}

/* Encontra um quadro livre */
static int find_free_frame() {
    for (int i = 0; i < pager.nframes; i++) {
        if (pager.frames[i].free) {
            return i;
        }
    }
    return -1; /* Nenhum quadro livre */
}

/* Encontra um bloco de disco livre */
static int find_free_block() {
    for (int i = 0; i < pager.nblocks; i++) {
        if (pager.free_blocks[i]) {
            pager.free_blocks[i] = 0; /* Marca como usado */
            pager.free_block_count--;
            return i;
        }
    }
    return -1; /* Sem blocos livres */
}

/* Libera um bloco de disco */
static void free_block(int block) {
    if (block >= 0 && block < pager.nblocks && !pager.free_blocks[block]) {
        pager.free_blocks[block] = 1;
        pager.free_block_count++;
    }
}

static int select_victim_frame() {
    int start = pager.clock_hand;

    while (1) {
        frame_entry_t *frame = &pager.frames[pager.clock_hand];

        if (!frame->free) {
            process_table_t *proc = find_process_table(frame->pid);
            if (proc && frame->page_index < proc->page_count) {
                page_entry_t *page = &proc->pages[frame->page_index];

                /* Só processa se a página está na memória */
                if (page->state == PAGE_IN_MEMORY) {
                    if (frame->referenced || page->referenced) {
                        /* Dá segunda chance: marca como não referenciada */
                        frame->referenced = 0;
                        page->referenced = 0;
                        
                        /* Se não está em PROT_NONE, coloca em PROT_NONE */
                        if (page->prot != PROT_NONE) {
                            void *vaddr = (void *)(UVM_BASEADDR +
                                                   frame->page_index * sysconf(_SC_PAGESIZE));
                            mmu_chprot(proc->pid, vaddr, PROT_NONE);
                            page->prot = PROT_NONE;
                        }
                    } else {
                        /* Encontrou vítima! */
                        int victim = pager.clock_hand;
                        pager.clock_hand = (pager.clock_hand + 1) % pager.nframes;
                        return victim;
                    }
                }
            }
        }

        pager.clock_hand = (pager.clock_hand + 1) % pager.nframes;
        
        /* Se deu uma volta completa e não encontrou vítima */
        if (pager.clock_hand == start) {
            /* Neste caso, todas as páginas já foram marcadas como PROT_NONE
               na passagem anterior. Agora escolhe a próxima como vítima. */
            int victim = pager.clock_hand;
            pager.clock_hand = (pager.clock_hand + 1) % pager.nframes;
            return victim;
        }
    }
}

static void evict_page(int frame) {

    frame_entry_t *f = &pager.frames[frame];
    if (f->free) return;

    process_table_t *proc = find_process_table(f->pid);
    if (!proc || f->page_index >= proc->page_count) {
        f->free = 1;
        return;
    }

    page_entry_t *page = &proc->pages[f->page_index];
    
    void *vaddr = (void *)(UVM_BASEADDR + f->page_index * sysconf(_SC_PAGESIZE));
    
    mmu_nonresident(f->pid, vaddr);

    /* Salva no disco apenas se a página estiver suja */
    if (page->dirty) {
        mmu_disk_write(frame, page->disk_block);
        page->dirty = 0;
        page->saved_on_disk = 1;  /* Disco tem dados válidos */
    } else {
        /* Se não está suja, não há dados válidos no disco */
        page->saved_on_disk = 0;
    }

    page->state = PAGE_ON_DISK;
    /* NÃO chama mmu_chprot aqui - já foi feito em select_victim_frame se necessário */

    f->free = 1;
    f->referenced = 0;
}

/* Traz uma página para a memória */
static void load_page(process_table_t *proc, int page_idx, int frame) {
    page_entry_t *page = &proc->pages[page_idx];
    frame_entry_t *f = &pager.frames[frame];
    page_state_t old_state = page->state;  /* Guarda estado anterior */

    f->free = 0;
    f->pid = proc->pid;
    f->page_index = page_idx;
    f->referenced = 1;

    page->frame = frame;
    page->state = PAGE_IN_MEMORY;
    page->referenced = 1;

    void *vaddr = (void *)(UVM_BASEADDR + page_idx * sysconf(_SC_PAGESIZE));

    if (old_state == PAGE_UNINITIALIZED) {
        mmu_zero_fill(frame);
        page->initialized = 1;
        page->saved_on_disk = 0;  /* Disco não tem dados válidos */
        page->dirty = 0;
    } else if (old_state == PAGE_ON_DISK) {
        if (page->saved_on_disk) {
            mmu_disk_read(page->disk_block, frame);
            page->dirty = 0;
            /* Mantém initialized=1 e saved_on_disk=1 */
        } else {
            mmu_zero_fill(frame);
            page->initialized = 1;
            page->saved_on_disk = 0;
            page->dirty = 0;
        }
    }
    /* Se já estava PAGE_IN_MEMORY, não faz nada */

    /* Sempre começa como somente leitura */
    mmu_resident(proc->pid, vaddr, frame, PROT_READ);
    page->prot = PROT_READ;
}

/* ==================== FUNÇÕES PÚBLICAS ==================== */

void pager_init(int nframes, int nblocks) {
    pthread_mutex_init(&pager.mutex, NULL);

    pager.nframes = nframes;
    pager.nblocks = nblocks;
    pager.clock_hand = 0;
    pager.processes = NULL;

    /* Aloca quadros físicos */
    pager.frames = malloc(nframes * sizeof(frame_entry_t));
    for (int i = 0; i < nframes; i++) {
        pager.frames[i].free = 1;
        pager.frames[i].referenced = 0;
    }

    /* Aloca blocos de disco */
    pager.free_blocks = malloc(nblocks * sizeof(int));
    for (int i = 0; i < nblocks; i++) {
        pager.free_blocks[i] = 1; /* Todos livres inicialmente */
    }
    pager.free_block_count = nblocks;
}

void pager_create(pid_t pid) {
    pthread_mutex_lock(&pager.mutex);

    /* Cria nova tabela de páginas para o processo */
    process_table_t *proc = create_process_table(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager.mutex);
        return;
    }

    pthread_mutex_unlock(&pager.mutex);
}

void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&pager.mutex);

    process_table_t *proc = find_process_table(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager.mutex);
        return NULL;
    }

    /* Verifica se há blocos de disco disponíveis */
    if (pager.free_block_count <= 0) {
        errno = ENOSPC;
        pthread_mutex_unlock(&pager.mutex);
        return NULL;
    }

    /* Aloca um bloco de disco */
    int block = find_free_block();
    if (block < 0) {
        pthread_mutex_unlock(&pager.mutex);
        return NULL;
    }

    /* Expande a tabela de páginas */
    int new_count = proc->page_count + 1;
    page_entry_t *new_pages = realloc(proc->pages, new_count * sizeof(page_entry_t));
    if (!new_pages) {
        free_block(block); /* Devolve bloco */
        pthread_mutex_unlock(&pager.mutex);
        return NULL;
    }

    proc->pages = new_pages;

    /* Inicializa nova página */
    page_entry_t *page = &proc->pages[proc->page_count];
    page->state = PAGE_UNINITIALIZED;
    page->frame = -1;
    page->disk_block = block;
    page->prot = PROT_NONE;
    page->referenced = 0;
    page->dirty = 0;
    page->initialized = 0;
    page->saved_on_disk = 0;  /* Nova flag: inicialmente disco não tem dados válidos */

    /* Calcula endereço virtual */
    void *vaddr = (void *)(UVM_BASEADDR + proc->page_count * sysconf(_SC_PAGESIZE));

    proc->page_count++;

    pthread_mutex_unlock(&pager.mutex);
    return vaddr;
}

void pager_fault(pid_t pid, void *addr) {
    pthread_mutex_lock(&pager.mutex);

    process_table_t *proc = find_process_table(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager.mutex);
        return;
    }

    intptr_t offset = (intptr_t)addr - UVM_BASEADDR;
    int page_idx = offset / sysconf(_SC_PAGESIZE);
    if (page_idx < 0 || page_idx >= proc->page_count) {
        pthread_mutex_unlock(&pager.mutex);
        return;
    }

    page_entry_t *page = &proc->pages[page_idx];
    void *page_vaddr = (void *)(UVM_BASEADDR + page_idx * sysconf(_SC_PAGESIZE));

    if (page->state == PAGE_IN_MEMORY) {
        page->referenced = 1;
        pager.frames[page->frame].referenced = 1;

        if (page->prot == PROT_NONE) {
            /* Foi dada segunda chance e a página voltou a ser usada */
            page->prot = PROT_READ;
            mmu_chprot(pid, page_vaddr, page->prot);
        } else if (page->prot == PROT_READ) {
            /* Falta por escrita em página só leitura */
            page->prot = PROT_READ | PROT_WRITE;
            page->dirty = 1;  /* MARCADA COMO SUJA! */
            mmu_chprot(pid, page_vaddr, page->prot);
        }

        pthread_mutex_unlock(&pager.mutex);
        return;
    }

    /* Página não está na memória: escolher quadro */
    int frame = find_free_frame();
    if (frame < 0) {
        frame = select_victim_frame();
        evict_page(frame);
    }

    load_page(proc, page_idx, frame);

    pthread_mutex_unlock(&pager.mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&pager.mutex);

    process_table_t *proc = find_process_table(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager.mutex);
        errno = EINVAL;
        return -1;
    }

    long pagesize = sysconf(_SC_PAGESIZE);

    /* Verifica se endereço está dentro do espaço alocado */
    intptr_t start_offset = (intptr_t)addr - UVM_BASEADDR;
    intptr_t end_offset   = start_offset + (intptr_t)len - 1;

    if (start_offset < 0 ||
        end_offset >= (intptr_t)proc->page_count * pagesize) {
        pthread_mutex_unlock(&pager.mutex);
        errno = EINVAL;
        return -1;
    }

    /* Para cada byte, imprime em hexadecimal */
    for (size_t i = 0; i < len; i++) {
        void *current_addr = (void *)((intptr_t)addr + (intptr_t)i);
        intptr_t offset = (intptr_t)current_addr - UVM_BASEADDR;
        int page_idx    = offset / pagesize;
        int byte_in_page = offset % pagesize;

        page_entry_t *page = &proc->pages[page_idx];

        /* Se página não está na memória, traz para memória (somente leitura) */
        if (page->state != PAGE_IN_MEMORY) {
            int frame = find_free_frame();
            if (frame < 0) {
                frame = select_victim_frame();
                evict_page(frame);
            }

            frame_entry_t *f = &pager.frames[frame];
            f->free       = 0;
            f->pid        = pid;
            f->page_index = page_idx;
            f->referenced = 1;

            page->frame      = frame;
            page->state      = PAGE_IN_MEMORY;
            page->referenced = 1;

            void *vaddr = (void *)(UVM_BASEADDR +
                                   (intptr_t)page_idx * pagesize);

            /* Usar a mesma lógica de load_page */
            if (page->state == PAGE_UNINITIALIZED) {
                mmu_zero_fill(frame);
                page->initialized = 1;
                page->saved_on_disk = 0;
                page->dirty = 0;
            } else if (page->state == PAGE_ON_DISK) {
                if (page->saved_on_disk) {
                    mmu_disk_read(page->disk_block, frame);
                    page->dirty = 0;
                } else {
                    mmu_zero_fill(frame);
                    page->initialized = 1;
                    page->saved_on_disk = 0;
                    page->dirty = 0;
                }
            }

            /* Mapeia como somente leitura para syslog */
            mmu_resident(pid, vaddr, frame, PROT_READ);
            page->prot = PROT_READ;
        }

        /* Atualiza bit de referência */
        page->referenced = 1;
        pager.frames[page->frame].referenced = 1;

        /* Lê byte da memória física e imprime */
        unsigned char byte =
            pmem[page->frame * pagesize + byte_in_page];
        printf("%02x", (unsigned)byte);
    }

    printf("\n");

    pthread_mutex_unlock(&pager.mutex);
    return 0;
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&pager.mutex);

    process_table_t *proc = find_process_table(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager.mutex);
        return;
    }

    /* Para cada página do processo */
    for (int i = 0; i < proc->page_count; i++) {
        page_entry_t *page = &proc->pages[i];

        /* Libera quadro físico se estiver ocupado */
        if (page->state == PAGE_IN_MEMORY) {
            pager.frames[page->frame].free = 1;
            pager.frames[page->frame].referenced = 0;
        }

        /* Libera bloco de disco */
        free_block(page->disk_block);
    }

    /* Remove tabela do processo */
    destroy_process_table(proc);

    pthread_mutex_unlock(&pager.mutex);
}
