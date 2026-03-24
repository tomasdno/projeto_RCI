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
#include "globals.h"

/* ================================================================
 * SERVIDOR UDP (comunicação com servidor de nós)
 * ================================================================ */
int cria_udp_socket(void) { // slides upd client
    int fd = socket(AF_INET, SOCK_DGRAM, 0); // cria um socket UDP usando IPv4
    if (fd == -1) { perror("socket UDP"); exit(1); }
    return fd; // retorna o file descriptor do socket criado
}

/* Transação UDP com timeout (2 s) ao servidor de nós.
   Devolve 0 se OK, -1 se erro/timeout. */
int udp_transacao(const char *pedido, char *resposta, int resp_size) { // slides udp client
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
int cria_tcp_servidor(const char *porto) {
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
int tcp_liga(const char *ip, const char *porto) {
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

/* Escrita completa num fd TCP */
int tcp_envia(int fd, const char *msg) { // envia a mensagem msg para o fd especificado
    size_t enviado = 0;
    size_t len = strlen(msg); // calcula o tamanho da mensagem a ser enviada (tive que adicionar)
    while (enviado < len) {  
        ssize_t n = write(fd, msg + enviado, len - enviado); // escreve parte da mensagem que falta enviar
        if (n <= 0) return -1; 
        enviado += (size_t)n; // atualiza a quantidade de bytes enviados
    }
    return 0;
}