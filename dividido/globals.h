/* 
    DESCRICAO DO FICHEIRO:
    Declaração de variáveis globais usadas em vários ficheiros do projeto.


*/

extern char my_ip[64]; // IP do nó (string)
extern char my_tcp[16]; // porto TCP onde o nó aceita ligações (string)
extern char reg_ip[64]; // IP do servidor de nós (string)
extern char reg_udp[16]; // porto UDP do servidor de nós (string)
extern char my_net[4]; // rede actual ("000"–"999")
extern char my_id[4]; // id actual ("00"–"99")
extern int  joined; // registo  
extern int listen_fd; // socket TCP de escuta
extern int udp_fd; // socket UDP (servidor nós)
extern Vizinho    vizinhos[MAX_VIZINHOS]; 
extern int        nb_count;
extern EntradaRota rota[MAX_DEST];
extern int         monitor_on;
extern ReadBuf rb[1024]; // buffer de leitura