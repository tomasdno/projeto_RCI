#define MAX_VIZINHOS   64       // máximo de vizinhos simultâneos   
#define MAX_DEST       100      // IDs de 00 a 99                   
#define BUF_SIZE       1024     // tamanho genérico de buffer        
#define CHAT_MAX       128      // máximo de carateres numa msg chat 

#define REG_IP_DFLT    "193.136.138.142" // IP do servidor de nós default
#define REG_UDP_DFLT   "59000" // porto UDP do servidor de nós default

#define INF  -1 // distância infinita (indica destino inalcançável)

/* --- Vizinho TCP --- */
typedef struct {
    int  fd;        /* descritor da sessão TCP          */
    char id[4];     /* identificador: "00" a "99"       */
    char ip[64];    /* endereço IP                      */
    char tcp[16];   /* porto TCP                        */
} Vizinho;

/* --- Estado de encaminhamento por destino --- */
typedef enum { EXPEDICAO = 0, COORDENACAO = 1 } EstadoRota; // estado de encaminhamento 
// EXPEDIÇÃO -> encontrar uma conexão de sucesso (ou caminho) para o destino e enviar mensagens ROUTE aos vizinhos
// COORDENAÇÃO -> nó já tem conhecimento de que não pode mais contar com um sucessor (reconfigurada a rota)
// o que removemos é o que fica em expedição 

typedef struct {
    int        dist;                    /* distância estimada (INF = ∞)     */
    int        succ;                    /* vizinho de expedição (-1 = nenhum)*/
    EstadoRota estado;                  // estado atual da rota expedição ou coordenação
    /* variáveis adicionais em estado de coordenação */
    int        succ_coord;              /* vizinho que causou coordenação    */
    int        coord[MAX_VIZINHOS];     /* coord[i]=1: coordenação em curso  */
} EntradaRota;

/* buffer de leitura parcial por fd (para mensagens TCP fragmentadas) */
typedef struct {
    char buf[BUF_SIZE * 4];
    int  len;
} ReadBuf;