/*
 * DESCRICAO DO FICHEIRO:
 * Declaração (extern) das variáveis globais partilhadas entre os vários
 * módulos do projecto. As definições encontram-se em main.c.
 */

extern char my_ip[64];   /* IP do próprio nó                  */
extern char my_tcp[16];  /* porto TCP de escuta               */
extern char reg_ip[64];  /* IP do servidor de nós             */
extern char reg_udp[16]; /* porto UDP do servidor de nós      */
extern char my_net[4];   /* rede actual ("000"–"999")         */
extern char my_id[4];    /* id actual   ("00"–"99")           */
extern int  joined;      /* 1 se registado na rede            */

extern int listen_fd;    /* socket TCP de escuta              */
extern int udp_fd;       /* socket UDP (servidor de nós)      */

extern Vizinho    vizinhos[MAX_VIZINHOS];
extern int        nb_count;

extern EntradaRota rota[MAX_DEST];
extern int         monitor_on;

extern ReadBuf rb[1024]; /* buffer de leitura indexado por fd */
