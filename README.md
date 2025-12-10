<!-- LTeX: language=pt-BR -->

# PAGINADOR DE MEMÓRIA -- RELATÓRIO

1. Termo de compromisso

    Ao entregar este documento preenchiso, os membros do grupo afirmam que todo o código desenvolvido para este trabalho é de autoria própria.  Exceto pelo material listado no item 3 deste relatório, os membros do grupo afirmam não ter copiado material da Internet nem ter obtido código de terceiros.

2. Membros do grupo e alocação de esforço

    Preencha as linhas abaixo com o nome e o email dos integrantes do grupo.  Substitua marcadores `XX` pela contribuição de cada membro do grupo no desenvolvimento do trabalho (os valores devem somar 100%).

    * Bianca Gabriela Franco e Silva <bgfes@ufmg.br> 50%
    * Gabriel Edmundo Souza Rocha <gabrielesr@ufmg.br> 50%

3. Referências bibliográficas

Consultamos o deepseek para tirar dúvidas sobre funções específicas e, de resto, fomos fazendo e testando com a ajuda do stackoverflow e demais fóruns de dúvidas.

5. Detalhes de implementação

    1. Descreva e justifique as estruturas de dados utilizadas em sua solução.

        Estruturas Principais:
       
        1. page_state_t (enum)
        typedef enum {
            PAGE_UNINITIALIZED,  /* Nunca acessada, sem quadro nem dados */
            PAGE_ON_DISK,        /* Em disco, não na memória */
            PAGE_IN_MEMORY       /* Na memória RAM */
        } page_state_t;

        Justificação: Modela o ciclo de vida completo de uma página:
            • PAGE_UNINITIALIZED: Páginas recém-alocadas (zero-fill-on-demand)
            • PAGE_ON_DISK: Páginas expulsas da RAM (swap)
            • PAGE_IN_MEMORY: Páginas ativamente mapeadas


        2. page_entry_t (struct)
        typedef struct {
            page_state_t state;      /* Estado atual */
            int frame;              /* Quadro físico (se em memória) */
            int disk_block;         /* Bloco de disco (swap) */
            int prot;               /* Permissões atuais */
            int referenced;         /* Bit de referência para LRU/clock */
            int dirty;              /* Modificada? (write-back) */
            int initialized;        /* Já foi zerada? */
            int saved_on_disk;      /* Disco tem dados válidos? */
        } page_entry_t;
        Justificação: Cada entrada contém:
            • Estado e localização (state, frame, disk_block): Rastreia onde os dados estão
            • Metadados de gerência (referenced, dirty): Para algoritmos de substituição
            • Otimizações (initialized, saved_on_disk): Evita operações desnecessárias


        3. process_table_t (struct)
        
        typedef struct process_table {
            pid_t pid;
            page_entry_t *pages;    /* Array dinâmico de páginas */
            int page_count;         /* Tamanho atual do espaço virtual */
            struct process_table *next; /* Lista encadeada */
        } process_table_t;
        Justificação:
            • Array dinâmico de páginas: Simples acesso O(1) por índice
            • Lista encadeada de processos: Suporta múltiplos processos concorrentes
            • PID como identificador: Compatível com sistema operacional


        4. frame_entry_t (struct)
        
        typedef struct {
            int free;           /* 1 se livre, 0 se ocupado */
            pid_t pid;          /* Processo dono */
            int page_index;     /* Índice da página no processo */
            int referenced;     /* Bit de referência (clock) */
        } frame_entry_t;
        Justificação: Tabela de quadros físicos inversa:
            • Localização reversa: Dado um quadro, encontra a página correspondente
            • Gerenciamento de memória física: Alocação/desalocação de quadros


        5. Estrutura Global pager
        
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
        Justificação:
            • Configuração flexível: Parâmetros definidos na inicialização
            • Bitmap para blocos de disco: Eficiente para gerenciar espaço swap
            • Algoritmo clock implementado: Substituição de páginas com segunda chance
            • Mutex para thread-safety: Operações atômicas em ambiente concorrente


        Decisões de Design:
            1. Separação clara entre estruturas: Páginas virtuais (page_entry_t), quadros físicos (frame_entry_t) e processos (process_table_t);
            2. Otimizações importantes: Saved_on_disk que evita ler do disco páginas nunca escritas, initialized que evita zero-fill repetido e free_block_count que é um contador rápido para verificação O(1);
            3. Escalabilidade: Lista encadeada para processos (número variável) e arrays para páginas e quadros (acesso rápido por índice).

    3. Descreva o mecanismo utilizado para controle de acesso e modificação às páginas.
  
       vou adicionar ao chegar em casa
