/*
 * OWR – OverlayWithRouting  (Rede Sobreposta com Encaminhamento)
 *
 * Cobre as etapas 2, 3 e 4 do ponto 4.1 do enunciado:
 *   Etapa 2: rede com 1 nó          -> join, leave
 *   Etapa 3: rede com vários nós    -> n, ae, sg, re, dj, dae
 *   Etapa 4: anúncio/encaminhamento -> a, sr, sm, em
 *
 * Uso: ./OWR IP TCP [regIP [regUDP]]
 *
 * DESCRICAO DO FICHEIRO:
 * Ponto de entrada do programa.  Inicializa os sockets, define variáveis
 * globais e executa o ciclo principal baseado em select() que monitoriza:
 *   - stdin (comandos do utilizador)
 *   - socket TCP de escuta (novas ligações de vizinhos)
 *   - sockets TCP dos vizinhos já ligados
 *   - socket UDP (respostas do servidor de nós, geridas em udp_transacao)
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

#include "structs.h"

/* ------------------------------------------------------------------ */
/* Funções externas                                                     */
/* ------------------------------------------------------------------ */
extern void adiciona_vizinho(int fd, const char *id, const char *ip, const char *tcp_port);
extern void processa_stdin(void);
extern int  le_vizinho(int fd);
extern void remove_vizinho(int idx);
extern int  cria_udp_socket(void);
extern int  cria_tcp_servidor(const char *porto);


/* ================================================================
 * ESTADO GLOBAL
 * ================================================================ */
char my_ip[64]   = "";
char my_tcp[16]  = "";
char reg_ip[64]  = REG_IP_DFLT;
char reg_udp[16] = REG_UDP_DFLT;
char my_net[4]   = "";
char my_id[4]    = "";
int  joined      = 0;

int listen_fd = -1;
int udp_fd    = -1;

Vizinho    vizinhos[MAX_VIZINHOS];
int        nb_count = 0;

EntradaRota rota[MAX_DEST];
int         monitor_on = 0;

ReadBuf rb[1024];  /* buffer de leitura parcial indexado por fd */


/* ================================================================
 * UTILITÁRIOS GERAIS
 * ================================================================ */

/* Ignora SIGPIPE para não terminar ao escrever num socket já fechado. */
static void setup_signals(void) {
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);
}

/* Devolve o maior fd activo (necessário para o primeiro argumento de select). */
static int max_fd(void) {
    int m = listen_fd;
    if (udp_fd > m) m = udp_fd;
    for (int i = 0; i < nb_count; i++)
        if (vizinhos[i].fd > m) m = vizinhos[i].fd;
    return m;
}


/* ================================================================
 * ACEITAR NOVA LIGAÇÃO TCP
 * ================================================================ */

/* Aceita uma ligação TCP entrante, regista o IP do cliente e adiciona
 * o novo vizinho com id temporário "??" até receber a mensagem NEIGHBOR. */
static void aceita_ligacao(void) {
    struct sockaddr addr;
    socklen_t addrlen = sizeof addr;
    int newfd = accept(listen_fd, &addr, &addrlen);
    if (newfd == -1) { perror("accept"); return; }

    char ip_str[64] = "?";
    if (addr.sa_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip_str, sizeof ip_str);
    }

    adiciona_vizinho(newfd, "??", ip_str, "?");
}


/* ================================================================
 * CICLO PRINCIPAL (select)
 * ================================================================ */

/* Loop infinito que aguarda actividade em qualquer fd registado e
 * despacha para o handler adequado. */
static void ciclo_principal(void) {
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);          /* stdin                  */
        FD_SET(listen_fd, &rfds);  /* novas ligações TCP     */
        if (udp_fd >= 0)
            FD_SET(udp_fd, &rfds); /* respostas UDP          */
        for (int i = 0; i < nb_count; i++)
            FD_SET(vizinhos[i].fd, &rfds);

        int ret = select(max_fd() + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        if (FD_ISSET(0, &rfds))
            processa_stdin();

        if (FD_ISSET(listen_fd, &rfds))
            aceita_ligacao();

        /* Percorre de trás para a frente para poder remover vizinhos
         * durante a iteração sem saltar entradas. */
        for (int i = nb_count - 1; i >= 0; i--) {
            if (FD_ISSET(vizinhos[i].fd, &rfds)) {
                if (le_vizinho(vizinhos[i].fd) < 0) {
                    printf("Ligação com %s fechada\n", vizinhos[i].id);
                    remove_vizinho(i);
                }
            }
        }
    }
}


/* ================================================================
 * MAIN
 * ================================================================ */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s IP TCP [regIP [regUDP]]\n", argv[0]);
        exit(1);
    }

    strncpy(my_ip,  argv[1], sizeof my_ip  - 1);
    strncpy(my_tcp, argv[2], sizeof my_tcp - 1);
    if (argc >= 4) strncpy(reg_ip,  argv[3], sizeof reg_ip  - 1);
    if (argc >= 5) strncpy(reg_udp, argv[4], sizeof reg_udp - 1);

    srand((unsigned)getpid());
    setup_signals();

    udp_fd    = cria_udp_socket();
    listen_fd = cria_tcp_servidor(my_tcp);

    printf("OWR iniciado: IP=%s  TCP=%s  regIP=%s  regUDP=%s\n",
           my_ip, my_tcp, reg_ip, reg_udp);
    printf("Digite 'help' para ver os comandos disponíveis.\n");

    ciclo_principal();

    close(listen_fd);
    close(udp_fd);
    return 0;
}
