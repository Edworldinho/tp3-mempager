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
    PAGE_UNINITIALIZED,
    PAGE_ON_DISK,
    PAGE_IN_MEMORY
} page_state_t;
```

---

#### 2. `page_entry_t` (struct)

```c
typedef struct {
    page_state_t state;
    int frame;
    int disk_block;
    int prot;
    int referenced;
    int dirty;
    int initialized;
    int saved_on_disk;
} page_entry_t;
```

---

#### 3. `process_table_t` (struct)

```c
typedef struct process_table {
    pid_t pid;
    page_entry_t *pages;
    int page_count;
    struct process_table *next;
} process_table_t;
```

---

#### 4. `frame_entry_t` (struct)

```c
typedef struct {
    int free;
    pid_t pid;
    int page_index;
    int referenced;
} frame_entry_t;
```

---

#### 5. Estrutura Global `pager`

```c
static struct {
    int nframes;
    int nblocks;
    frame_entry_t *frames;
    int *free_blocks;
    int free_block_count;
    process_table_t *processes;
    int clock_hand;
    pthread_mutex_t mutex;
} pager;
```

---

## Mecanismo de Controle de Acesso e Modificação

### 1. Sistema de Permissões

```c
int prot;  /* PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE */
```

---

### 2. Implementação do Controle

#### Falta de Página (`pager_fault`)

```c
if (page->state == PAGE_IN_MEMORY) {
    if (page->prot == PROT_NONE) {
        page->prot = PROT_READ;
        mmu_chprot(pid, page_vaddr, page->prot);
    } else if (page->prot == PROT_READ) {
        page->prot = PROT_READ | PROT_WRITE;
        page->dirty = 1;
        mmu_chprot(pid, page_vaddr, page->prot);
    }
}
```

---

#### Algoritmo Clock

```c
if (page->prot != PROT_NONE) {
    void *vaddr = ...;
    mmu_chprot(proc->pid, vaddr, PROT_NONE);
    page->prot = PROT_NONE;
}
```

---

### 3. Controle de Modificação

```c
page->prot = PROT_READ | PROT_WRITE;
page->dirty = 1;
```

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

#### `syslog`

```c
mmu_resident(pid, vaddr, frame, PROT_READ);
page->prot = PROT_READ;
```

#### Nova Página

```c
mmu_resident(proc->pid, vaddr, frame, PROT_READ);
page->prot = PROT_READ;
```

---

## Vantagens

* Proteção de acesso
* Write-back eficiente
* Zero-fill-on-demand
* Segunda chance com PROT_NONE
* `saved_on_disk` mantém consistência
* `dirty` garante persistência

---

## Fluxo Típico de Acesso

```
1. Acesso READ → page fault
2. Carrega com PROT_READ
3. Escrita → page fault
4. PROT_WRITE + dirty=1
5. Clock:
   referenced=1 → PROT_NONE
   referenced=0 → expulsão
   dirty=1 → write-back
```

---

## Decisões de Design

* Separação entre estruturas
* Otimizações (`saved_on_disk`, `initialized`, `free_block_count`)
* Estruturas escaláveis

---

# Seção de sugestões que geram Pontos Extras

## 1. Melhorias de especificação

### Explicar Comportamento de `pager_syslog` com Strings Cruzando Múltiplas Páginas (no doc de especificação)

## Detalhamento de `pager_syslog`

A função `pager_syslog` deve:

1. Aceitar valores de `len` arbitrários (incluindo > PAGESIZE)
2. Tratar strings que cruzam múltiplas páginas automaticamente
3. Verificar se **todos os bytes** de `addr` a `addr+len-1` estão em páginas alocadas
4. Retornar **-1 (EINVAL)** se qualquer byte estiver fora do espaço alocado
5. Acessar cada página necessária (trazendo para memória se estiver em disco)

---

## 2. Melhorias na Documentação do Código

### Documentação das Estruturas de Dados

## Arquitetura do Sistema (adicionar essa explicação sobre esses componentes e o fluxo da página, em um info.MD)

### Componentes:

```
1. **Aplicação (testX.c)**: usa uvm.h para alocar memória
2. **UVM (uvm.c)**: gerencia sinais SIGSEGV e comunicação
3. **MMU (mmu.c)**: servidor central, gerencia memória física
4. **Pager (pager.c)**: implementa políticas de paginação
```

### Fluxo de Falha de Página:

```
App → SIGSEGV → uvm_segv_action → SEGV_REQ → mmu.c → pager_fault
          ↓
App continua ← SEGV_REP ← mmu_resident ← pager.c
```

---

## 3. Identificação de Erros nas Bibliotecas

### a) Race Condition em `uvm_segv_action`

```c
void uvm_segv_action(int signum, siginfo_t *si, void *context) {
    pthread_mutex_lock(&uvm->mutex);
    pthread_cond_wait(&uvm->cond, &uvm->mutex);
    pthread_mutex_unlock(&uvm->mutex);
}
```

#### Correção sugerida

```c
// Usa pthread_mutex_trylock com timeout
void uvm_segv_action(int signum, siginfo_t *si, void *context) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1; // timeout de 1 segundo

    if (pthread_mutex_timedlock(&uvm->mutex, &ts) != 0) {
        // Não conseguiu lock, aborta o processo
        fprintf(stderr, "Cannot handle segfault: mutex busy\n");
        exit(EXIT_FAILURE);
    }
}
```

---

### b) Memory Leak em `mmu_client_destroy`

```c
void mmu_client_destroy(struct mmu_client *c) {
    if(c->pid) {
        pager_destroy(c->pid);
    }
}
```

#### Correção sugerida

```c
void mmu_client_destroy(struct mmu_client *c) {
    mmu->sock2client[c->sock] = NULL;
    c->running = 0;
    close(c->sock);
    if(c->pid) {
        pager_destroy(c->pid);
    }
    free(c); // Impede um memory Leak
}
```

---

### c) Potencial Deadlock em `mmu_resident` / `mmu_nonresident`

```c
do {
    if(recv(c->sock, &t, sizeof(t), MSG_PEEK) != sizeof(t))
        goto out_client;
} while(t != MMU_PROTO_REMAP_REQ); // LOOP INFINITO SE CLIENTE MORRER
```

#### Correção sugerida

```c
int attempts = 0;

do {
    if(recv(c->sock, &t, sizeof(t), MSG_PEEK) != sizeof(t))
        goto out_client;

    attempts++;
    if (attempts > 100) { //Timeout após 100 tentativas
        logd(LOG_ERROR, "mmu_resident: timeout waiting for REMAP_REQ\n");
        goto out_client;
    }

    usleep(10000);
} while(t != MMU_PROTO_REMAP_REQ);
```
