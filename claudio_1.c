/*
 * OWR - OverlayWithRouting
 * Rede Sobreposta com Encaminhamento
 *
 * Cobre as etapas 2, 3 e 4 do ponto 4.1 do enunciado:
 *   Etapa 2: rede com 1 nó  -> comandos join, leave
 *   Etapa 3: rede com vários nós -> comandos show nodes (n), add edge (ae),
 *                                   show neighbors (sg), remove edge (re),
 *                                   direct join (dj), direct add edge (dae)
 *   Etapa 4: anúncio e encaminhamento -> comandos announce (a),
 *                                        show routing (sr), start monitor (sm),
 *                                        end monitor (em)
 *
 * Compilar: gcc OWR.c -o OWR
 * Usar:     ./OWR IP TCP [regIP [regUDP]]
 *
 * Exemplo:  ./OWR 192.168.1.10 58100
 *           ./OWR 192.168.1.10 58100 193.136.138.142 59000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ================================================================
 * CONSTANTES
 * ================================================================ */
#define MAX_VIZINHOS   64       // máximo de vizinhos simultâneos   
#define MAX_DEST       100      // IDs de 00 a 99                   
#define BUF_SIZE       1024     // tamanho genérico de buffer        
#define CHAT_MAX       128      // máximo de carateres numa msg chat 

#define REG_IP_DFLT    "193.136.138.142" // IP do servidor de nós default
#define REG_UDP_DFLT   "59000" // porto UDP do servidor de nós default

#define INF  -1 // distância infinita (indica destino inalcançável)

/* ================================================================
 * ESTRUTURAS DE DADOS
 * ================================================================ */

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

/* ================================================================
 * ESTADO GLOBAL
 * ================================================================ */
static char my_ip[64]   = ""; // IP do nó (string)
static char my_tcp[16]  = ""; // porto TCP onde o nó aceita ligações (string)
static char reg_ip[64]  = REG_IP_DFLT; // IP do servidor de nós (string)
static char reg_udp[16] = REG_UDP_DFLT; // porto UDP do servidor de nós (string)
static char my_net[4]   = "";   // rede actual ("000"–"999") 
static char my_id[4]    = "";   // id actual   ("00"–"99")  
static int  joined      = 0;    // 1 se estiver registado    *

static int listen_fd = -1;      /* socket TCP de escuta      */
static int udp_fd    = -1;      /* socket UDP (servidor nós) */

static Vizinho    vizinhos[MAX_VIZINHOS]; // lista de vizinhos TCP conectados (máximo MAX_VIZINHOS)
static int        nb_count = 0; // número de vizinhos atualmente conectados

static EntradaRota rota[MAX_DEST]; // array da struct array EntradaRota 
static int         monitor_on = 0;   // flag para indicar se o monitor de mensagens está ativo (1) ou não (0)

/* buffer de leitura parcial por fd (para mensagens TCP fragmentadas) */
typedef struct {
    char buf[BUF_SIZE * 4];
    int  len;
} ReadBuf;
static ReadBuf rb[1024];   /* indexado pelo fd */

/* ================================================================
 * UTILITÁRIOS GERAIS
 * ================================================================ */

/* Ignora o programa tenta escrever num socket ou pipe que já foi fechado pelo outro lado para não morrer ao escrever num socket fechado */
static void setup_signals(void) {
    struct sigaction act;// estrutura para especificar ação a tomar em caso de sinal
    memset(&act, 0, sizeof act); // inicializa a estrutura com zeros
    act.sa_handler = SIG_IGN; // 
    sigaction(SIGPIPE, &act, NULL);
}
    // struct sigaction act;
    // act.sa_handler = handler;
    // sigemptyset(&sa.sa_mask);
    // act.sa_flags = 0;
    // sigaction(SIGINT, &act, NULL);


/* Encontra vizinho pelo fd */
static int vizinho_por_fd(int fd) { // percorre a lista de vizinhos para encontrar o índice do vizinho com o fd especificado
    for (int i = 0; i < nb_count; i++)  
        if (vizinhos[i].fd == fd) return i; // se encontrar o vizinho com o fd correspondente, retorna o índice
    return -1;
}

/* Encontra vizinho pelo id */
static int vizinho_por_id(const char *id) {
    for (int i = 0; i < nb_count; i++)
        if (strcmp(vizinhos[i].id, id) == 0) return i;
    return -1;
}

/* Calcula fd máximo para o select() */
static int max_fd(void) {
    int m = listen_fd; // começa com o fd do socket TCP de escuta
    if (udp_fd > m) m = udp_fd; // compara com o fd do socket UDP e atualiza se for maior
    for (int i = 0; i < nb_count; i++) // percorre a lista de vizinhos para encontrar o maior fd dos mesmos
        if (vizinhos[i].fd > m) m = vizinhos[i].fd; // comparação do maiuor fd ja encontrado
    /* stdin = 0, sempre menor */
    return m; // maior fd encontrado
}

/* Escrita completa num fd TCP */
static int tcp_envia(int fd, const char *msg) { // envia a mensagem msg para o fd especificado
    size_t enviado = 0;
    int len = strlen(msg); // calcula o tamanho da mensagem a ser enviada (tive que adicionar)
    while (enviado < len) {  
        ssize_t n = write(fd, msg + enviado, len - enviado); // escreve parte da mensagem que falta enviar
        if (n <= 0) return -1; 
        enviado += (size_t)n; // atualiza a quantidade de bytes enviados
    }
    return 0;
}

/* ================================================================
 * INICIALIZAÇÃO DE ENCAMINHAMENTO
 * ================================================================ */
static void rota_init(void) {
    for (int i = 0; i < MAX_DEST; i++) { // inicializa a tabela de roteamento para cada destino
        rota[i].dist       = INF;
        rota[i].succ       = -1;
        rota[i].estado     = EXPEDICAO;
        rota[i].succ_coord = -1;
        memset(rota[i].coord, 0, sizeof rota[i].coord);
    }
    /* O nó conhece-se a si próprio com distância 0 */
    int id_int = atoi(my_id); // conversao string para int
    if (id_int >= 0 && id_int < MAX_DEST) { // se o id do nó for válido, inicializa a rota para si próprio
        rota[id_int].dist  = 0;
        rota[id_int].succ  = id_int;  /* sucessor = si próprio */
    }
}

/* ================================================================
 * SERVIDOR UDP (comunicação com servidor de nós)
 * ================================================================ */
static int cria_udp_socket(void) { // slides upd client
    int fd = socket(AF_INET, SOCK_DGRAM, 0); // cria um socket UDP usando IPv4
    if (fd == -1) { perror("socket UDP"); exit(1); }
    return fd; // retorna o file descriptor do socket criado
}

/* Transação UDP com timeout (2 s) ao servidor de nós.
   Devolve 0 se OK, -1 se erro/timeout. */
static int udp_transacao(const char *pedido, char *resposta, int resp_size) { // slides udp client
    struct addrinfo hints, *res; //
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(reg_ip, reg_udp, &hints, &res) != 0) { // obtém as informações de endereço do servidor de nós usando o IP e porto configurados
        fprintf(stderr, "Erro: getaddrinfo servidor de nós\n");
        return -1;
    }

    ssize_t n = sendto(udp_fd, pedido, strlen(pedido), 0,
                       res->ai_addr, res->ai_addrlen); // envia o pedido para o servidor de nós usando o socket UDP
    freeaddrinfo(res);
    if (n == -1) { perror("sendto"); return -1; }

    /* Aguarda resposta com timeout de 2 s */
    fd_set rfds; // 
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(udp_fd, &rfds);
    tv.tv_sec  = 2; // tempo maximo de espera por resposta do servidor de nós 2 segundos
    tv.tv_usec = 0; // + tempo em microsegundos

    int r = select(udp_fd + 1, &rfds, NULL, NULL, &tv); // aguarda até que o socket UDP esteja pronto para leitura (resposta do servidor de nós) ou até que o timeout ocorra
    if (r <= 0) {
        fprintf(stderr, "Timeout aguardando resposta do servidor de nós\n");
        return -1;
    }
    n = recvfrom(udp_fd, resposta, resp_size - 1, 0, NULL, NULL); // lê a resposta do servidor de nós para o buffer resposta, garantindo que haja espaço para a terminação de string
    if (n <= 0) { perror("recvfrom"); return -1; }
    resposta[n] = '\0'; // adiciona terminação de string à resposta recebida
    return 0;
}

/* ================================================================
 * SERVIDOR TCP (aceitar ligações de vizinhos)
 * ================================================================ */
static int cria_tcp_servidor(const char *porto) {
    struct addrinfo hints, *res; //struct com informações para socket TCP
    int fd, yes = 1;

    // slides tcp server
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, porto, &hints, &res) != 0) {
        perror("getaddrinfo TCP servidor"); exit(1);
    }
    fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd == -1) { perror("socket TCP servidor"); exit(1); }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes); //

    if (bind(fd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("bind TCP servidor"); exit(1);
    }
    if (listen(fd, 5) == -1) {
        perror("listen TCP servidor"); exit(1);
    }
    freeaddrinfo(res);
    return fd;
}

/* ================================================================
 * CLIENTE TCP (ligar a vizinho)
 * ================================================================ */
static int tcp_liga(const char *ip, const char *porto) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ip, porto, &hints, &res) != 0) {
        fprintf(stderr, "Erro: getaddrinfo %s:%s\n", ip, porto);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd == -1) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ================================================================
 * MENSAGENS DE ENCAMINHAMENTO – ENVIO
 * ================================================================ */
static void envia_route_todos(int dest, int dist) { //msg de rotta para todos os vizinhos com destino dest, desitancia dist 
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist); //msg de rota para o destino dest com distância dist
    for (int i = 0; i < nb_count; i++) { // envia a mensagem de rota para todos os vizinhos conectados
        if (monitor_on)
            printf("[MONITOR] -> %s: %s", vizinhos[i].id, msg);
        tcp_envia(vizinhos[i].fd, msg); //msg rota para vizinho i com destino dest e distância dist
    }
}

static void envia_route_a(int nb_idx, int dest, int dist) { // msg para vizinho nb_idx com destino dest e distância dist
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist); // msg de rota para o vizinho nb_idx com destino dest e distância dist
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg); //imprime msg rota caso monitor esteja ativo
    tcp_envia(vizinhos[nb_idx].fd, msg); //msg de rota para vizinho[nb_idx] com destino dest e distância dist
}

static void envia_coord_a(int nb_idx, int dest) { // msg para vizinho nb_idx com destino dest 
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "COORD %02d\n", dest); // msg coordenação destino dest
    if (monitor_on) 
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg); 
    tcp_envia(vizinhos[nb_idx].fd, msg); // envia a msg de coordenação para o vizinho nb_idx
}

static void envia_uncoord_a(int nb_idx, int dest) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "UNCOORD %02d\n", dest);
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg);
    tcp_envia(vizinhos[nb_idx].fd, msg);
}

/* ================================================================
 * PROTOCOLO DE ENCAMINHAMENTO – RECEÇÃO DE MENSAGENS
 * ================================================================ */

/* Receção de ROUTE dest n vindo do vizinho from_nb */
static void recebe_route(int from_nb, int dest, int n) { //msg rota, destino dest, distancia n, recebida do vizinho from_nb
    if (monitor_on)
        printf("[MONITOR] <- %s: ROUTE %02d %d\n",
               vizinhos[from_nb].id, dest, n);

    /* Ignora anúncios do próprio nó */
    if (dest == atoi(my_id)) return; // se o destino da msg de rota for o próprio nó, ignora a mensagem (não processa rotas para si próprio)
    if (dest < 0 || dest >= MAX_DEST) return; // erro caso destino seja inválido

    int nova_dist = n + 1; // nova distancia para o dest pelo vizinho from_nb (distancia anunciada pelo vizinho + 1 para contar a aresta até o vizinho)
    if (rota[dest].dist == INF || nova_dist < rota[dest].dist) { // nova distância calculada for menor do que a distância atual, atualiza a rota para esse destino
        rota[dest].dist = nova_dist;
        rota[dest].succ = atoi(vizinhos[from_nb].id); // atualiza o sucessor para o destino dest como o vizinho from_nb (o vizinho que anunciou a rota mais curta)
        if (rota[dest].estado == EXPEDICAO) //se dest = expedição, envia msg de rota para todos os vizinhos com a nova distância para esse destino
            envia_route_todos(dest, rota[dest].dist);
    }
}

/* Receção de COORD dest vindo do vizinho from_nb */
static void recebe_coord(int from_nb, int dest) {
    if (monitor_on)
        printf("[MONITOR] <- %s: COORD %02d\n", vizinhos[from_nb].id, dest);

    if (dest < 0 || dest >= MAX_DEST) return;
    int j = atoi(vizinhos[from_nb].id);

    if (rota[dest].estado == COORDENACAO) {
        /* já em coordenação -> responde logo com UNCOORD */
        envia_uncoord_a(from_nb, dest);
        return;
    }

    /* estado == EXPEDICAO */
    if (j != rota[dest].succ) {
        /* não é o meu sucessor -> envio rota + uncoord */
        if (rota[dest].dist != INF)
            envia_route_a(from_nb, dest, rota[dest].dist);
        envia_uncoord_a(from_nb, dest);
    } else {
        /* é o meu sucessor -> entro em coordenação */
        rota[dest].estado     = COORDENACAO;
        rota[dest].succ_coord = rota[dest].succ;
        rota[dest].dist       = INF;
        rota[dest].succ       = -1;
        /* envia COORD a todos os vizinhos */
        for (int k = 0; k < nb_count; k++) {
            rota[dest].coord[k] = 1;
            envia_coord_a(k, dest);
        }
    }
}

/* Receção de UNCOORD dest vindo do vizinho from_nb */
static void recebe_uncoord(int from_nb, int dest) {
    if (monitor_on)
        printf("[MONITOR] <- %s: UNCOORD %02d\n", vizinhos[from_nb].id, dest);

    if (dest < 0 || dest >= MAX_DEST) return;

    if (rota[dest].estado == COORDENACAO) {
        rota[dest].coord[from_nb] = 0;

        /* verifica se toda a coordenação terminou */
        int todos_zero = 1;
        for (int k = 0; k < nb_count; k++)
            if (rota[dest].coord[k]) { todos_zero = 0; break; }

        if (todos_zero) {
            rota[dest].estado = EXPEDICAO;
            if (rota[dest].dist != INF)
                envia_route_todos(dest, rota[dest].dist);
            if (rota[dest].succ_coord != -1) {
                /* encontra índice do succ_coord e envia UNCOORD */
                char sc_str[4];
                snprintf(sc_str, sizeof sc_str, "%02d", rota[dest].succ_coord);
                int idx = vizinho_por_id(sc_str);
                if (idx >= 0)
                    envia_uncoord_a(idx, dest);
            }
        }
    }
}

/* ================================================================
 * GESTÃO DE VIZINHOS
 * ================================================================ */

/* Adiciona vizinho já com fd e id conhecidos */
static void adiciona_vizinho(int fd, const char *id,
                              const char *ip, const char *tcp_port) {
    if (nb_count >= MAX_VIZINHOS) { // verificação nº max vizinhos atingida
        fprintf(stderr, "Erro: número máximo de vizinhos atingido\n");
        close(fd);
        return;
    }
    int i = nb_count++; // incrementa nº de vizinhos e guarda o índice do novo vizinho
    //guardar os dados do novo vizinho com nº i na struct vizinhos: fd, id, ip e porto tcp
    vizinhos[i].fd = fd;
    strncpy(vizinhos[i].id,  id,       sizeof vizinhos[i].id  - 1);
    strncpy(vizinhos[i].ip,  ip,       sizeof vizinhos[i].ip  - 1);
    strncpy(vizinhos[i].tcp, tcp_port, sizeof vizinhos[i].tcp - 1);
    memset(&rb[fd], 0, sizeof rb[fd]);

    /* Ao adicionar um novo vizinho:
       - se estiver em EXPEDICAO para algum destino, envia ROUTE ao novo viz.
       - se estiver em COORDENACAO, o novo viz. não é dependência (coord=0) */
    for (int d = 0; d < MAX_DEST; d++) {
        rota[d].coord[i] = 0;   /* por omissão não é dependência */
        if (rota[d].estado == EXPEDICAO && rota[d].dist != INF) // se o destino d estiver em estado de expedição e a distância para esse destino for diferente de infinito, envia uma mensagem ROUTE para o novo vizinho com a distância atual para esse destino
            envia_route_a(i, d, rota[d].dist); // msg route parar viz i com destino d e distancia rota[d].dist
    }
    /* Se ainda não sabemos o ID (??), adiamos a mensagem de adição até receber NEIGHBOR */
    if (strcmp(id, "??") != 0)
        printf("Vizinho %s adicionado (fd=%d)\n", id, fd);
}

/* Remove vizinho por índice */
static void remove_vizinho(int idx) {
    int dest_fd = vizinhos[idx].fd; // fd do vizinho a remover
    printf("Vizinho %s removido\n", vizinhos[idx].id); 
    close(dest_fd); // fecha a conexão TCP com o vizinho a remover

    /* Protocolo de encaminhamento: remoção de aresta */
    for (int d = 0; d < MAX_DEST; d++) { // percorre todos os destinos para verificar se o vizinho removido é o sucessor de algum destino
        if (rota[d].estado == EXPEDICAO) { // se o destino estiver em estado de expedição, verifica se o sucessor é o vizinho removido
            if (rota[d].succ == atoi(vizinhos[idx].id)) { // se o sucessor do destino for o vizinho removido, precisamos entrar em coordenação
                /* sucessor perdido -> entra em coordenação */
                rota[d].estado     = COORDENACAO;
                rota[d].succ_coord = -1;  // remove a dependencia de sucessor
                rota[d].dist       = INF; // distância passa a ser infinita durante a coordenação
                rota[d].succ       = -1; // sucessor passa a ser desconhecido durante a coordenação (TEMPORARIO)
                /* envia COORD a todos os outros vizinhos */
                for (int k = 0; k < nb_count; k++) { // coordenação com os outros vizinhos (exceto o removido)
                    if (k == idx) continue; // não envia coordenação para o vizinho removido
                    rota[d].coord[k] = 1; // //espera de coordenação do vizinho k para o destino d
                    envia_coord_a(k, d); // envia mensagem de coordenação para os outros vizinhos
                }
                /* se não há outros vizinhos, volta a EXPEDICAO  */
                int tem_outros = 0;
                for (int k = 0; k < nb_count; k++) // verifica se há outros vizinhos além do removido que estão em coordenação para o destino d
                    if (k != idx && rota[d].coord[k]) { tem_outros = 1; break; } // se encontrar algum viznho em coordenação para d aumenta a flag tem_outros
                if (!tem_outros) rota[d].estado = EXPEDICAO; // sem vizinhos, volta o estado de encaminhamento EXPEDICAO
            }
        } else {
            /* estado COORDENACAO: o vizinho removido já não é dependência */
            rota[d].coord[idx] = 0; // se o destino d estiver em estado de coordenação, o vizinho removido não é mais dependência, então marca a coordenação com esse vizinho como 0 (sem dependência)
        }
    }

    /* Remove da lista de vizinhos (swap com último) */
    memset(&rb[dest_fd], 0, sizeof rb[dest_fd]); // clear buffer de leitura do fd do vizinho removido
    vizinhos[idx] = vizinhos[--nb_count];  // remove vizinho da lista
}

/* ================================================================
 * PROCESSAMENTO DE MENSAGENS RECEBIDAS DE UM VIZINHO TCP
 * ================================================================ */
static void processa_msg_vizinho(int nb_idx, const char *linha) {
    char tipo[32]; // string com tipo de msg 
    if (sscanf(linha, "%31s", tipo) != 1) return; // erro

    if (strcmp(tipo, "NEIGHBOR") == 0) {
        /* NEIGHBOR id  – identificação do vizinho */
        char id[4];
        if (sscanf(linha, "NEIGHBOR %3s", id) == 1) {
            char old_id[4];
            strncpy(old_id, vizinhos[nb_idx].id, sizeof old_id - 1);
            old_id[sizeof old_id - 1] = '\0';
            strncpy(vizinhos[nb_idx].id, id, sizeof vizinhos[nb_idx].id - 1);
            vizinhos[nb_idx].id[sizeof vizinhos[nb_idx].id - 1] = '\0';
            if (strcmp(old_id, id) != 0) {
                if (strcmp(old_id, "??") == 0) {
                    printf("Vizinho %s adicionado (fd=%d)\n",
                           id, vizinhos[nb_idx].fd);
                } else {
                    printf("Vizinho %s identificado como %s (fd=%d)\n",
                           old_id, id, vizinhos[nb_idx].fd);
                }
            }
        }
    }
    else if (strcmp(tipo, "ROUTE") == 0) {
        int dest, n;
        if (sscanf(linha, "ROUTE %d %d", &dest, &n) == 2)
            recebe_route(nb_idx, dest, n);
    }
    else if (strcmp(tipo, "COORD") == 0) {
        int dest;
        if (sscanf(linha, "COORD %d", &dest) == 1)
            recebe_coord(nb_idx, dest);
    }
    else if (strcmp(tipo, "UNCOORD") == 0) {
        int dest;
        if (sscanf(linha, "UNCOORD %d", &dest) == 1)
            recebe_uncoord(nb_idx, dest);
    }
    else if (strcmp(tipo, "CHAT") == 0) {
        /* CHAT origin dest mensagem */
        int origin, dest;
        char mensagem[CHAT_MAX + 1] = "";
        if (sscanf(linha, "CHAT %d %d %128[^\n]", &origin, &dest, mensagem) >= 2) {
            int my_id_int = atoi(my_id);
            if (dest == my_id_int) {
                printf("[CHAT] De %02d: %s\n", origin, mensagem);
            } else {
                /* encaminhar */
                if (rota[dest].dist != INF && rota[dest].succ >= 0) {
                    char succ_str[4];
                    snprintf(succ_str, sizeof succ_str, "%02d", rota[dest].succ);
                    int succ_idx = vizinho_por_id(succ_str);
                    if (succ_idx >= 0) {
                        char fwd[BUF_SIZE];
                        snprintf(fwd, sizeof fwd, "CHAT %02d %02d %s\n",
                                 origin, dest, mensagem);
                        tcp_envia(vizinhos[succ_idx].fd, fwd);
                    } else {
                        printf("Erro: sem rota para %02d\n", dest);
                    }
                } else {
                    printf("Erro: sem rota para %02d\n", dest);
                }
            }
        }
    }
    else {
        fprintf(stderr, "Mensagem desconhecida: %s\n", linha);
    }
}

/* Lê dados de um fd TCP e processa linha a linha */
static int le_vizinho(int fd) {
    if (fd < 0 || fd >= 1024) return -1; // verificação de fd válido para acessar o buffer de leitura
    ReadBuf *b = &rb[fd]; // ponteiro para o buffer de leitura associado ao fd especificado
    ssize_t n = read(fd, b->buf + b->len, sizeof(b->buf) - b->len - 1); // le dados fd, armazena no buffer de leitura a partir da posicao b->len
    if (n <= 0) return -1;   /* conexão fechada ou erro */
    b->len += (int)n; // atualiza o comprimento dos dados lidos no buffer
    b->buf[b->len] = '\0'; // adicona terminação de strung

    /* Processa todas as linhas completas (terminadas em '\n') */
    char *ptr = b->buf; // ponteiro para percorrer o buffer de leitura (inicio do buffer)
    char *nl; // ponteiro para encontrar a posição da próxima nova linha no buffer 

    while ((nl = memchr(ptr, '\n', b->buf + b->len - ptr)) != NULL) { //percorre o buffer inteiro ate \n
        *nl = '\0'; // susbstitui o \n por \0 para conseguir usar isso como string
        int nb_idx = vizinho_por_fd(fd); // encontrar o indice do viz que corresponde ao fd que enviou a mensagem
        if (nb_idx >= 0) // caso encontre  o vizinho que queremos, processa a msg recebida   
            processa_msg_vizinho(nb_idx, ptr);
        ptr = nl + 1; // atualiza o ponteiro para o início da próxima linha a ser processada (após o \n)
    }
    /* Move dados restantes para o início do buffer */
    int restante = (int)(b->buf + b->len - ptr); // quantidade de dados restantes buffer 
    memmove(b->buf, ptr, restante); // 
    b->len = restante;
    return 0;
}

/* ================================================================
 * ACEITAR NOVA LIGAÇÃO TCP
 * ================================================================ */
static void aceita_ligacao(void) {
    struct sockaddr addr; // estrutura para armazenar o endereço do cliente que está tentando se conectar
    // socket addr nao tem campo para ip e porto mas tem campo sa_family para indicar o tipo de endereço e campo genérico sa_data onde estao os  outros dados
    socklen_t addrlen = sizeof addr; // variável para armazenar o tamanho do endereço do cliente, necessário para a função accept
    int newfd = accept(listen_fd, &addr, &addrlen); // aceita a nova conexão TCP, criando um novo socket para comunicação com o cliente e preenchendo a estrutura addr com as informações do cliente
    if (newfd == -1) { perror("accept"); return; } 

    /* Obtém IP do cliente */
    char ip_str[64] = "?";
    if (addr.sa_family == AF_INET) { // verificação de IPv4
        struct sockaddr_in *sa = (struct sockaddr_in *)&addr; // converção de sa_data para sin_family, sin_addr e sin port
        inet_ntop(AF_INET, &sa->sin_addr, ip_str, sizeof ip_str); // converte o endereço IP do cliente para uma string legível e armazena em ip_str
    }

    /* Adiciona temporariamente com id "??" até receber NEIGHBOR */
    adiciona_vizinho(newfd, "??", ip_str, "?");
}

/* ================================================================
 * COMANDOS: ETAPA 2 – join / leave
 * ================================================================ */

/* join net id  OU  direct join net id */
static void cmd_join(const char *net, const char *id, int directo) {
    if (joined) { // se já estiver em uma rede, não pode se juntar a outra sem sair primeiro
        printf("Já está na rede %s com id %s. Use 'leave' primeiro.\n",
               my_net, my_id);
        return;
    }
    strncpy(my_net, net, sizeof my_net - 1);
    strncpy(my_id,  id,  sizeof my_id  - 1);
    joined = 1;
    rota_init(); //inicia tabela de roteamento para o nó, definindo a distância para si próprio como 0 e o sucessor como si próprio

    if (directo) {
        printf("Direct join: rede=%s id=%s (sem registo no servidor)\n",
               my_net, my_id);
        return;
    }

    /* REG tid 0 net id IP TCP\n */
    int tid = rand() % 1000; // gera tdi aleatório para a transação de registro
    char pedido[BUF_SIZE], resposta[BUF_SIZE]; // buffers para armazenar pedido de registo e resposta do serbior de nós
    snprintf(pedido, sizeof pedido,
             "REG %03d 0 %s %s %s %s\n", tid, my_net, my_id, my_ip, my_tcp);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) { //se udp tiver sucesso, processa a resposta do servidor de nós
        int rtid, rop; // armazenar tid de resposta e código de operação da resposta do servidor de nós
        // rop -> 1 = registo bem sucedido, 2 = erro base de dados cheia, outros valores = erro no registo
        if (sscanf(resposta, "REG %d %d", &rtid, &rop) == 2) { 
            if (rop == 1)
                printf("Registado na rede %s com id %s\n", my_net, my_id);
            else if (rop == 2)
                printf("Erro: base de dados cheia\n");
            else
                printf("Erro no registo (op=%d)\n", rop);
        } else {
            printf("Resposta inesperada: %s\n", resposta);
        }
    }
}

// leave -> remove todas as arestas, envia pedido de remoção ao servidor de nós e limpa estado local
static void cmd_leave(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }

    /* Fecha todas as arestas */
    while (nb_count > 0) // passa por todos os vizinhos e remove um por um até não ter mais vizinhos
        remove_vizinho(0);

    /* REG tid 3 net id\n  (pedido de remoção) */
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "REG %03d 3 %s %s\n", tid, my_net, my_id);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) {
        int rtid, rop;
        // rop -> 4 = remoção bem sucedida, outros valores = erro na remoção
        if (sscanf(resposta, "REG %d %d", &rtid, &rop) == 2) {
            if (rop == 4)
                printf("Registo removido da rede %s\n", my_net);
            else
                printf("Resposta ao leave (op=%d)\n", rop);
        }
    }

    joined = 0;
    my_net[0] = '\0';
    my_id[0]  = '\0';
    printf("Saiu da rede.\n");
}

/* ================================================================
 * COMANDOS: ETAPA 3 – show nodes, add edge, remove edge,
 *                     show neighbors, direct add edge
 * ================================================================ */

/* show nodes (n) net */
static void cmd_show_nodes(const char *net) {
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido, "NODES %03d 0 %s\n", tid, net);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return; // se servidor de nós não responder, retorna sem processar a resposta

    /* Resposta: "NODES tid 1 net\nid1\nid2\n..." */
    int rtid, rop; // tid resposta e operacao de resposts
    char rnet[4]; // nome da rede na resposta do servidor de nós
    char *linha = resposta; // ponteiro para percorrer a resposta do servidor de nós linha a linha
    /* Lê cabeçalho */
    char *nl = strchr(linha, '\n'); // encontra \n
    if (!nl) { printf("Resposta mal formada\n"); return; }
    *nl = '\0';
    if (sscanf(linha, "NODES %d %d %3s", &rtid, &rop, rnet) != 3 || rop != 1) { //rop = 1 -> sucesso, outros valores -> erro
        printf("Erro ao obter nós da rede %s\n", net);
        return;
    }
    printf("Nós na rede %s:\n", net);
    linha = nl + 1; // atualiza o ponteiro para a próxima linha após o cabeçalho
    // iterar pela resposta do servidor de nós linha a linha, imprimindo os ids dos nós encontrados ao encontrar um \n, substitui por \0 para conseguir usar como string e imprime o id do nó
    while (*linha) {
        nl = strchr(linha, '\n');
        if (nl) *nl = '\0';
        if (strlen(linha) > 0)
            printf("  %s\n", linha);
        if (!nl) break;
        linha = nl + 1;
    }
}

/* add edge (ae) id  –  obtém contacto via servidor de nós e liga */
static void cmd_add_edge(const char *id) { //
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) { // se já existe um vizinho com o id especificado, não pode adicionar uma nova aresta para esse id
        printf("Já existe aresta com %s\n", id); return;
    }

    /* CONTACT tid 0 net id\n */
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "CONTACT %03d 0 %s %s\n", tid, my_net, id);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return; //erro caso servidor de nos nao responda

    int rtid, rop; 
    char rnet[4], rid[4], rip[64], rtcp[16]; // rnet -> nome da rede na resposta do servidor de nós, rid -> id do nó encontrado, rip -> ip do nó encontrado, rtcp -> porto tcp do nó encontrado
    if (sscanf(resposta, "CONTACT %d %d %3s %3s %63s %15s",
               &rtid, &rop, rnet, rid, rip, rtcp) < 4) {
        printf("Resposta mal formada\n"); return;
    }
    if (rop == 2) {
        printf("Nó %s não está registado na rede %s\n", id, my_net); return;
    }
    if (rop != 1) {
        printf("Erro ao obter contacto (op=%d)\n", rop); return;
    }

    /* Liga por TCP */
    int fd = tcp_liga(rip, rtcp); // conexão tcp entre o nó atual e o nó encontrado com ip rip e porto rtcp
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", rip, rtcp); return; }

    /* Envia NEIGHBOR my_id */
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) { // envia mensagem de vizinhança para o nó encontrado, caso haja erro no envio, fecha a conexão tcp e retorna
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, rip, rtcp); // adiciona o no encontrado como vizinho, com o fd da conexão tcp, id do nó encontrado, ip e porto tcp do nó encontrado
}

/* direct add edge (dae) id idIP idTCP */
static void cmd_direct_add_edge(const char *id, const char *ip,
                                 const char *tcp_port) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) { // se já existe um vizinho com o id especificado, não pode adicionar uma nova aresta para esse id
        printf("Já existe aresta com %s\n", id); return;
    }

    int fd = tcp_liga(ip, tcp_port); // conexão tcp entre o nó atual e o nó com ip e porto tcp especificados
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", ip, tcp_port); return; }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) { // envia mensagem para todos ps vizinhos com o id do nó atual, caso haja erro no envio, fecha a conexão tcp e retorna
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, ip, tcp_port); //adiciona o nó como vizinho, com o fd da conexão tcp, id do nó, ip e porto tcp especificados
}

/* remove edge (re) id */
static void cmd_remove_edge(const char *id) { // remove a aresta para o vizinho com o id especificado
    int idx = vizinho_por_id(id); // encontra o índice do vizinho com o id especificado
    if (idx < 0) { printf("Não existe aresta com %s\n", id); return; } // caso nao encontre, da erro
    remove_vizinho(idx); // remove o vizinho encontrado, que fecha a conexão tcp e atualiza as rotas para os destinos que tinham esse vizinho como sucessor, entrando em coordenação se necessário
}

/* show neighbors (sg) */
static void cmd_show_neighbors(void) { 
    if (nb_count == 0) {
        printf("Sem vizinhos.\n"); return;
    }
    printf("Vizinhos:\n");
    for (int i = 0; i < nb_count; i++) //percorre lista de vizinhos que esta no vetor na porsição i = 0 até ao nº de vizinhos nb_count, imprimindo o id, ip e porto tcp de cada vizinho
        printf("  id=%-3s  ip=%-20s  tcp=%s\n",
               vizinhos[i].id, vizinhos[i].ip, vizinhos[i].tcp);
}

/* ================================================================
 * COMANDOS: ETAPA 4 – announce, show routing, monitor
 * ================================================================ */

/* announce (a) – anuncia o próprio nó a todos os vizinhos */
static void cmd_announce(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    int id_int = atoi(my_id); // converção de string do id do no para int
    /* Distância 0 a si próprio */
    rota[id_int].dist = 0;
    rota[id_int].succ = id_int;
    /* Envia ROUTE id 0 a todos os vizinhos */
    envia_route_todos(id_int, 0); // msg de rota para vizinhos com destino id_int (o próprio nó) e distância 0
    printf("Anúncio enviado (sou %s)\n", my_id);
}

/* show routing (sr) dest */
static void cmd_show_routing(const char *dest_str) {
    int dest = atoi(dest_str); // converção de string do destino para int
    if (dest < 0 || dest >= MAX_DEST) { // verificação de destino válido
        printf("Destino inválido\n"); return;
    }
    if (rota[dest].estado == COORDENACAO) { //
        printf("Destino %02d: estado=COORDENACAO\n", dest);
    } else {
        if (rota[dest].dist == INF) //se dist for inf, entao nao ha rota esse para o destino
            printf("Destino %02d: estado=EXPEDICAO  dist=INF  succ=NENHUM\n", dest);
        else
            printf("Destino %02d: estado=EXPEDICAO  dist=%d  succ=%02d\n",
                   dest, rota[dest].dist, rota[dest].succ); // se dist for diferente de inf, imprime a distância e o sucessor para esse destino
    }
}

/* start monitor / end monitor */
static void cmd_start_monitor(void) { monitor_on = 1; printf("Monitor ON\n"); }
static void cmd_end_monitor(void)   { monitor_on = 0; printf("Monitor OFF\n"); }

/* ================================================================
 * PROCESSAMENTO DE COMANDOS DO UTILIZADOR (stdin)
 * ================================================================ */
static void processa_stdin(void) {
    char linha[BUF_SIZE]; // buffer da linha de comando
    if (!fgets(linha, sizeof linha, stdin)) { // ler a linha stdin
        /* EOF no stdin -> sai */
        if (joined) cmd_leave();
        exit(0);
    }
    /* Remove \n */
    linha[strcspn(linha, "\n")] = '\0';
    if (strlen(linha) == 0) return;

    char cmd[32], a1[128], a2[128], a3[BUF_SIZE]; // buffers para comando e argumentos
    a1[0] = a2[0] = a3[0] = '\0'; 
    sscanf(linha, "%31s %127s %127s %1023[^\n]", cmd, a1, a2, a3); // cmd->comando a1->id a2->ip a3->tcp_port 

    /* --- Etapa 2 --- */
    if (strcmp(cmd, "join") == 0 || strcmp(cmd, "j") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0)
            printf("Uso: join net id\n");
        else
            cmd_join(a1, a2, 0); 
    }
    else if (strcmp(cmd, "leave") == 0 || strcmp(cmd, "l") == 0) {
        cmd_leave();
    }
    else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "x") == 0) {
        if (joined) cmd_leave();
        exit(0);
    }
    /* --- Etapa 3 --- */
    else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "show") == 0) {
        /* "show nodes net"  ou  "n net" */
        if (strcmp(cmd, "show") == 0 && strcmp(a1, "nodes") == 0)
            cmd_show_nodes(a2);
        else if (strcmp(cmd, "n") == 0)
            cmd_show_nodes(a1);
        else
            printf("Comando desconhecido\n");
    }
    else if (strcmp(cmd, "ae") == 0) {
        if (strlen(a1) == 0) printf("Uso: ae id\n");
        else cmd_add_edge(a1);
    }
    else if (strcmp(cmd, "re") == 0) {
        if (strlen(a1) == 0) printf("Uso: re id\n");
        else cmd_remove_edge(a1);
    }
    else if (strcmp(cmd, "sg") == 0) {
        cmd_show_neighbors();
    }
    else if (strcmp(cmd, "dj") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0)
            printf("Uso: dj net id\n");
        else
            cmd_join(a1, a2, 1);
    }
    else if (strcmp(cmd, "dae") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0 || strlen(a3) == 0)
            printf("Uso: dae id idIP idTCP\n");
        else
            cmd_direct_add_edge(a1, a2, a3);
    }
    /* --- Etapa 4 --- */
    else if (strcmp(cmd, "a") == 0) {
        cmd_announce();
    }
    else if (strcmp(cmd, "sr") == 0) {
        if (strlen(a1) == 0) printf("Uso: sr dest\n");
        else cmd_show_routing(a1);
    }
    else if (strcmp(cmd, "sm") == 0) {
        cmd_start_monitor();
    }
    else if (strcmp(cmd, "em") == 0) {
        cmd_end_monitor();
    }
    /* --- Ajuda --- */
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
        printf("Comandos disponíveis:\n");
        printf("  j/join net id         -- entrar na rede\n");
        printf("  l/leave               -- sair da rede\n");
        printf("  x/exit                -- fechar aplicação\n");
        printf("  n net                 -- listar nós da rede\n");
        printf("  ae id                 -- adicionar aresta\n");
        printf("  re id                 -- remover aresta\n");
        printf("  sg                    -- mostrar vizinhos\n");
        printf("  dj net id             -- entrar (sem servidor)\n");
        printf("  dae id ip tcp         -- add aresta (sem servidor)\n");
        printf("  a                     -- anunciar nó\n");
        printf("  sr dest               -- mostrar rota para dest\n");
        printf("  sm                    -- activar monitor\n");
        printf("  em                    -- desactivar monitor\n");
    }
    else {
        printf("Comando desconhecido: '%s'. Digite 'help'.\n", cmd);
    }
}

/* ================================================================
 * CICLO PRINCIPAL (select) ver slides guia utilização select
 * ================================================================ */
static void ciclo_principal(void) {
    while (1) {
        fd_set rfds; //select 
        FD_ZERO(&rfds); // coemçar com conjunto vazio
        FD_SET(0, &rfds);   // 
        FD_SET(listen_fd, &rfds);  //
        if (udp_fd >= 0)
            FD_SET(udp_fd, &rfds);      /* UDP (respostas do servidor de nós) */
        for (int i = 0; i < nb_count; i++)
            FD_SET(vizinhos[i].fd, &rfds);

        int mfd = max_fd(); // encontra o maior fd para passar ao select
        int ret = select(mfd + 1, &rfds, NULL, NULL, NULL); // espera por atividade em algum fd
        if (ret < 0) { // erro do select
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        /* stdin */
        if (FD_ISSET(0, &rfds)) // se há atividade no stdin
            processa_stdin();

        /* nova ligação TCP */
        if (FD_ISSET(listen_fd, &rfds)) // se há uma nova ligação chegando no socket de escuta TCP
            aceita_ligacao();

        /* vizinhos TCP */
        for (int i = nb_count - 1; i >= 0; i--) {
            if (FD_ISSET(vizinhos[i].fd, &rfds)) {
                if (le_vizinho(vizinhos[i].fd) < 0) {
                    printf("Ligação com %s fechada\n", vizinhos[i].id);
                    remove_vizinho(i);
                }
            }
        }

        /* socket UDP (não usado aqui em escuta, mas previsto) */
        /* (as respostas UDP são lidas dentro de udp_transacao com select) */
    }
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(int argc, char *argv[]) {
    if (argc < 3) { // IP TCP [regIP [regUDP]]
        fprintf(stderr, "Uso: %s IP TCP [regIP [regUDP]]\n", argv[0]);
        exit(1);
    }

    strncpy(my_ip,  argv[1], sizeof my_ip  - 1); // -1 por causa do \n
    strncpy(my_tcp, argv[2], sizeof my_tcp - 1); 
    if (argc >= 4) strncpy(reg_ip,  argv[3], sizeof reg_ip  - 1);
    if (argc >= 5) strncpy(reg_udp, argv[4], sizeof reg_udp - 1);

    srand((unsigned)getpid()); // para gerar tid aleatórios
    setup_signals();           // ???????????????

    /* Cria socket UDP para comunicação com servidor de nós */
    udp_fd = cria_udp_socket(); // 

    /* Cria servidor TCP de escuta */
    listen_fd = cria_tcp_servidor(my_tcp); // cria o socket TCP com o porto my_tcp

    printf("OWR iniciado: IP=%s  TCP=%s  regIP=%s  regUDP=%s\n",
           my_ip, my_tcp, reg_ip, reg_udp);
    printf("Digite 'help' para ver os comandos disponíveis.\n");

    ciclo_principal();

    close(listen_fd); // fecha o socket de escuta TCP
    close(udp_fd); // fecha o socket UDP
    return 0; 
}