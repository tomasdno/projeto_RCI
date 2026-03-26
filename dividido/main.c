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

#include "structs.h"

/* 
    DESCRICAO DO FICHEIRO:
    Core inicial do programa
    - Função main para inicializar o programa, configurar sockets e iniciar o ciclo principal
    - Ciclo principal usando select para monitorar stdin, socket de escuta TCP, socket UDP e conexões TCP com vizinhos
 -??--- Configuração de sinais para ignorar SIGPIPE
    - Função select
    - Função para aceitar novas conexões TCP de vizinhos
    - Função para calcular o maior fd para passar ao select
    

*/

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


// Funçoes externas
extern void adiciona_vizinho(int fd, const char *id, const char *ip, const char *tcp_port);
extern void processa_stdin(void);
extern int le_vizinho(int fd);
extern void remove_vizinho(int idx);
extern int cria_udp_socket(void);
extern int cria_tcp_servidor(const char *porto);

/* ================================================================
 * ESTADO GLOBAL
 * ================================================================ */
char my_ip[64]   = ""; // IP do nó (string)
char my_tcp[16]  = ""; // porto TCP onde o nó aceita ligações (string)
char reg_ip[64]  = REG_IP_DFLT; // IP do servidor de nós (string)
char reg_udp[16] = REG_UDP_DFLT; // porto UDP do servidor de nós (string)
char my_net[4]   = "";   // rede actual ("000"–"999") 
char my_id[4]    = "";   // id actual   ("00"–"99")  
int  joined      = 0;    // 1 se estiver registado    

int listen_fd = -1;      /* socket TCP de escuta      */
int udp_fd    = -1;      /* socket UDP (servidor nós) */

Vizinho    vizinhos[MAX_VIZINHOS]; // lista de vizinhos TCP conectados (máximo MAX_VIZINHOS)
int        nb_count = 0; // número de vizinhos atualmente conectados

EntradaRota rota[MAX_DEST]; // array da struct array EntradaRota 
int         monitor_on = 0;   // flag para indicar se o monitor de mensagens está ativo (1) ou não (0)

/* buffer de leitura parcial por fd (para mensagens TCP fragmentadas) */
ReadBuf rb[1024];   /* indexado pelo fd */

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

/* Calcula fd máximo para o select() */
static int max_fd(void) {
    int m = listen_fd; // começa com o fd do socket TCP de escuta
    if (udp_fd > m) m = udp_fd; // compara com o fd do socket UDP e atualiza se for maior
    for (int i = 0; i < nb_count; i++) // percorre a lista de vizinhos para encontrar o maior fd dos mesmos
        if (vizinhos[i].fd > m) m = vizinhos[i].fd; // comparação do maiuor fd ja encontrado
    /* stdin = 0, sempre menor */
    return m; // maior fd encontrado
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