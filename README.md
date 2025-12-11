# Paginador de Memória — Relatório

## Termo de Compromisso

Ao entregar este documento preenchido, os membros do grupo afirmam que todo o código desenvolvido para este trabalho é de autoria própria.
Exceto pelo material listado no item **Referências Bibliográficas**, os membros do grupo afirmam não ter copiado material da Internet nem ter obtido código de terceiros.

---

## Membros do Grupo e Alocação de Esforço

* **Bianca Gabriela Franco e Silva** — [bgfes@ufmg.br](mailto:bgfes@ufmg.br) — **50%**
* **Gabriel Edmundo Souza Rocha** — [gabrielesr@ufmg.br](mailto:gabrielesr@ufmg.br) — **50%**

---

## Referências Bibliográficas

Consultamos o DeepSeek para tirar dúvidas sobre funções específicas e utilizamos StackOverflow e demais fóruns de dúvidas.

---

## Detalhes de Implementação

### Estruturas de Dados Utilizadas

#### 1. `page_state_t` (enum)

```c
typedef enum {
    PAGE_UNINITIALIZED,  /* Nunca acessada, sem quadro nem dados */
    PAGE_ON_DISK,        /* Em disco, não na memória */
    PAGE_IN_MEMORY       /* Na memória RAM */
} page_state_t;
```

Modela o ciclo de vida de uma página:

* **PAGE_UNINITIALIZED:** páginas recém-alocadas (zero-fill-on-demand)
* **PAGE_ON_DISK:** páginas expulsas da RAM (swap)
* **PAGE_IN_MEMORY:** páginas ativamente mapeadas

---

#### 2. `page_entry_t` (struct)

```c
typedef struct {
    page_state_t state;      /* Estado atual */
    int frame;               /* Quadro físico (se em memória) */
    int disk_block;          /* Bloco de disco (swap) */
    int prot;                /* Permissões atuais */
    int referenced;          /* Bit de referência para LRU/clock */
    int dirty;               /* Modificada? (write-back) */
    int initialized;         /* Já foi zerada? */
    int saved_on_disk;       /* Disco tem dados válidos? */
} page_entry_t;
```

* Estado e localização (`state`, `frame`, `disk_block`)
* Metadados de gerência (`referenced`, `dirty`)
* Otimizações (`initialized`, `saved_on_disk`)

---

#### 3. `process_table_t` (struct)

```c
typedef struct process_table {
    pid_t pid;
    page_entry_t *pages;        /* Array dinâmico de páginas */
    int page_count;             /* Tamanho do espaço virtual */
    struct process_table *next; /* Lista encadeada */
} process_table_t;
```

* Array dinâmico para acesso O(1)
* Lista encadeada para múltiplos processos
* PID como identificador padrão

---

#### 4. `frame_entry_t` (struct)

```c
typedef struct {
    int free;           /* 1 se livre, 0 se ocupado */
    pid_t pid;          /* Processo dono */
    int page_index;     /* Índice da página no processo */
    int referenced;     /* Bit de referência (clock) */
} frame_entry_t;
```

* Tabela inversa de quadros
* Facilita substituição de páginas e mapeamento reverso

---

#### 5. Estrutura Global `pager`

```c
static struct {
    int nframes;                /* Número de quadros físicos */
    int nblocks;                /* Número de blocos de disco */
    frame_entry_t *frames;      /* Tabela de quadros físicos */
    int *free_blocks;           /* Bitmap de blocos livres */
    int free_block_count;       /* Contador rápido */
    process_table_t *processes; /* Lista de processos */
    int clock_hand;             /* Ponteiro do algoritmo clock */
    pthread_mutex_t mutex;      /* Sincronização */
} pager;
```

* Gerenciamento global de memória
* Bitmap eficiente para swap
* Algoritmo clock implementado
* Mutex para thread safety

---

## Mecanismo de Controle de Acesso e Modificação

### 1. Sistema de Permissões em Três Níveis

```c
int prot;  /* PROT_NONE, PROT_READ, ou PROT_READ | PROT_WRITE */
```

* **PROT_NONE:** Acesso revogado
* **PROT_READ:** Somente leitura
* **PROT_READ | PROT_WRITE:** Leitura e escrita

---

### 2. Implementação do Controle

#### A. Falta de Página (`pager_fault`)

```c
if (page->state == PAGE_IN_MEMORY) {
    if (page->prot == PROT_NONE) {
        /* Segunda chance: reativa para leitura */
        page->prot = PROT_READ;
        mmu_chprot(pid, page_vaddr, page->prot);
    } else if (page->prot == PROT_READ) {
        /* Upgrade para escrita */
        page->prot = PROT_READ | PROT_WRITE;
        page->dirty = 1;  /* Marca como modificada */
        mmu_chprot(pid, page_vaddr, page->prot);
    }
}
```

---

#### B. Algoritmo Clock com Controle de Acesso

```c
if (page->prot != PROT_NONE) {
    void *vaddr = ...;
    mmu_chprot(proc->pid, vaddr, PROT_NONE);
    page->prot = PROT_NONE;
}
```

---

### 3. Controle de Modificação (Dirty Bit)

#### Marcação

```c
page->prot = PROT_READ | PROT_WRITE;
page->dirty = 1;
```

#### Write-back no Swap

```c
if (page->dirty) {
    mmu_disk_write(frame, page->disk_block);
    page->dirty = 0;
    page->saved_on_disk = 1;
} else {
    page->saved_on_disk = 0;
}
```

---

### 4. Políticas Especiais

#### Páginas para `syslog`

```c
mmu_resident(pid, vaddr, frame, PROT_READ);
page->prot = PROT_READ;
```

#### Nova Página (`load_page`)

```c
mmu_resident(proc->pid, vaddr, frame, PROT_READ);
page->prot = PROT_READ;
```

---

## Vantagens do Mecanismo

* Processos só escrevem onde têm permissão
* Write-back eficiente (somente páginas sujas)
* Zero-fill-on-demand
* Segunda chance com PROT_NONE
* `saved_on_disk` evita dados inválidos
* `dirty` garante persistência

---

## Fluxo Típico de Acesso

```
1. Acesso READ → PROT_NONE ou não residente → page fault
2. Página carregada com PROT_READ
3. Escrita → page fault
4. Upgrade para PROT_WRITE, dirty = 1
5. Algoritmo clock:
   a. referenced=1 → PROT_NONE
   b. referenced=0 → expulsão
   c. dirty=1 → write-back
```

---

## Decisões de Design

* Separação entre páginas virtuais, quadros físicos e processos
* Otimizações com `saved_on_disk`, `initialized` e `free_block_count`
* Estruturas escaláveis: lista de processos + arrays de páginas e quadros
