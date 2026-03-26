/*
 * DESCRICAO DO FICHEIRO:
 * Declaração de constantes, estruturas e tipos usados em todo o projeto.
 * Inclui a estrutura de vizinhos TCP, a tabela de roteamento por destino
 * e o buffer de leitura parcial para lidar com mensagens TCP fragmentadas.
 */

#define MAX_VIZINHOS   64       /* máximo de vizinhos simultâneos   */
#define MAX_DEST       100      /* IDs de 00 a 99                   */
#define BUF_SIZE       1024     /* tamanho genérico de buffer        */
#define CHAT_MAX       128      /* máximo de carateres numa msg chat */

#define REG_IP_DFLT    "193.136.138.142"     /* 192.168.1.1 */
#define REG_UDP_DFLT   "59000"

#define INF  -1  /* distância infinita (indica destino inalcançável) */


/* --- Vizinho TCP ---
 * Representa uma ligação TCP activa com um nó vizinho.
 * O campo id é preenchido após receber a mensagem NEIGHBOR. */
typedef struct {
    int  fd;        /* descritor da sessão TCP    */
    char id[4];     /* identificador: "00" a "99" */
    char ip[64];    /* endereço IP                */
    char tcp[16];   /* porto TCP                  */
} Vizinho;


/* --- Estado de encaminhamento por destino ---
 * EXPEDICAO  : rota válida conhecida; mensagens ROUTE são propagadas.
 * COORDENACAO: sucessor perdido; aguarda UNCOORD de todos os vizinhos
 *              antes de voltar a EXPEDICAO. */
typedef enum { EXPEDICAO = 0, COORDENACAO = 1 } EstadoRota;

typedef struct {
    int        dist;                /* distância estimada (INF = ∞)        */
    int        succ;                /* vizinho de expedição (-1 = nenhum)  */
    EstadoRota estado;              /* estado actual da rota               */
    int        succ_coord;          /* vizinho que desencadeou coordenação */
    int        coord[MAX_VIZINHOS]; /* coord[i]=1: aguarda UNCOORD de i    */
} EntradaRota;


/* --- Buffer de leitura parcial por fd ---
 * Usado para reconstituir mensagens TCP que chegam fragmentadas. */
typedef struct {
    char buf[BUF_SIZE * 4];
    int  len;
} ReadBuf;
