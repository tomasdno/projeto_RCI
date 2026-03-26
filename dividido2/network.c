/*
 * DESCRICAO DO FICHEIRO:
 * Funções de comunicação de rede, cobrindo:
 *   - Criação do socket UDP e transações UDP com timeout para comunicar
 *     com o servidor de nós.
 *   - Criação do socket TCP de servidor (bind + listen).
 *   - Criação de ligações TCP de cliente (connect).
 *   - Envio fiável de mensagens TCP, tratando escritas parciais.
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
#include "globals.h"


/* ================================================================
 * SOCKET UDP – comunicação com o servidor de nós
 * ================================================================ */

/* Cria e devolve um socket UDP (IPv4). */
int cria_udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) { perror("socket UDP"); exit(1); }
    return fd;
}

/* Envia 'pedido' ao servidor de nós e aguarda resposta até 2 segundos.
 * Devolve 0 em caso de sucesso, -1 em caso de erro ou timeout. */
int udp_transacao(const char *pedido, char *resposta, int resp_size) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(reg_ip, reg_udp, &hints, &res) != 0) {
        fprintf(stderr, "Erro: getaddrinfo servidor de nós\n");
        return -1;
    }

    ssize_t n = sendto(udp_fd, pedido, strlen(pedido), 0,
                       res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (n == -1) { perror("sendto"); return -1; }

    /* Aguarda resposta com timeout de 2 s */
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(udp_fd, &rfds);
    tv.tv_sec  = 2;
    tv.tv_usec = 0;

    int r = select(udp_fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) {
        fprintf(stderr, "Timeout aguardando resposta do servidor de nós\n");
        return -1;
    }

    n = recvfrom(udp_fd, resposta, resp_size - 1, 0, NULL, NULL);
    if (n <= 0) { perror("recvfrom"); return -1; }
    resposta[n] = '\0';
    return 0;
}


/* ================================================================
 * SOCKET TCP – servidor (aceitar ligações de vizinhos)
 * ================================================================ */

/* Cria, faz bind e coloca em listen o socket TCP de escuta no porto
 * indicado. Termina o processo em caso de erro. */
int cria_tcp_servidor(const char *porto) {
    struct addrinfo hints, *res;
    int fd, yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, porto, &hints, &res) != 0) {
        perror("getaddrinfo TCP servidor"); exit(1);
    }
    fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd == -1) { perror("socket TCP servidor"); exit(1); }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

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
 * SOCKET TCP – cliente (ligar a um vizinho)
 * ================================================================ */

/* Cria uma ligação TCP ao endereço ip:porto indicado.
 * Devolve o fd em caso de sucesso, -1 em caso de erro. */
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


/* ================================================================
 * ENVIO TCP FIÁVEL
 * ================================================================ */

/* Envia a mensagem completa 'msg' pelo fd TCP, tratando escritas parciais.
 * Devolve 0 em caso de sucesso, -1 em caso de erro. */
int tcp_envia(int fd, const char *msg) {
    size_t enviado = 0;
    size_t len = strlen(msg);
    while (enviado < len) {
        ssize_t n = write(fd, msg + enviado, len - enviado);
        if (n <= 0) return -1;
        enviado += (size_t)n;
    }
    return 0;
}
