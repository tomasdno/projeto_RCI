/*
 * DESCRICAO DO FICHEIRO:
 * Implementação do protocolo de encaminhamento, incluindo:
 *   - Inicialização da tabela de roteamento (distância 0 para o próprio nó).
 *   - Envio de mensagens ROUTE, COORD e UNCOORD para vizinhos.
 *   - Processamento de mensagens ROUTE, COORD e UNCOORD recebidas,
 *     actualizando a tabela e gerindo transições entre EXPEDICAO e
 *     COORDENACAO.
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

/* ------------------------------------------------------------------ */
/* Funções externas                                                     */
/* ------------------------------------------------------------------ */
extern int tcp_envia(int fd, const char *msg);
extern int vizinho_por_id(const char *id);


/* ================================================================
 * INICIALIZAÇÃO DA TABELA DE ROTEAMENTO
 * ================================================================ */

/* Inicializa todos os destinos a INF sem sucessor.
 * O próprio nó fica com distância 0 e sucessor = si próprio. */
void rota_init(void) {
    for (int i = 0; i < MAX_DEST; i++) {
        rota[i].dist       = INF;
        rota[i].succ       = -1;
        rota[i].estado     = EXPEDICAO;
        rota[i].succ_coord = -1;
        memset(rota[i].coord, 0, sizeof rota[i].coord);
    }
    int id_int = atoi(my_id);
    if (id_int >= 0 && id_int < MAX_DEST) {
        rota[id_int].dist = 0;
        rota[id_int].succ = id_int;
    }
}


/* ================================================================
 * ENVIO DE MENSAGENS DE ENCAMINHAMENTO
 * ================================================================ */

/* Envia ROUTE dest dist a todos os vizinhos actualmente ligados. */
void envia_route_todos(int dest, int dist) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist);
    for (int i = 0; i < nb_count; i++) {
        if (monitor_on)
            printf("[MONITOR] -> %s: %s", vizinhos[i].id, msg);
        tcp_envia(vizinhos[i].fd, msg);
    }
}

/* Envia ROUTE dest dist apenas ao vizinho nb_idx. */
void envia_route_a(int nb_idx, int dest, int dist) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist);
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg);
    tcp_envia(vizinhos[nb_idx].fd, msg);
}

/* Envia COORD dest ao vizinho nb_idx. */
void envia_coord_a(int nb_idx, int dest) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "COORD %02d\n", dest);
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg);
    tcp_envia(vizinhos[nb_idx].fd, msg);
}

/* Envia UNCOORD dest ao vizinho nb_idx. */
void envia_uncoord_a(int nb_idx, int dest) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "UNCOORD %02d\n", dest);
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg);
    tcp_envia(vizinhos[nb_idx].fd, msg);
}


/* ================================================================
 * RECEÇÃO DE MENSAGENS DE ENCAMINHAMENTO
 * ================================================================ */

/* Processa ROUTE dest n recebido do vizinho from_nb.
 * Se a nova distância (n+1) for melhor que a actual, actualiza a rota
 * e propaga ROUTE a todos os vizinhos (quando em EXPEDICAO). */
void recebe_route(int from_nb, int dest, int n) {
    if (monitor_on)
        printf("[MONITOR] <- %s: ROUTE %02d %d\n",
               vizinhos[from_nb].id, dest, n);

    if (dest == atoi(my_id)) return;  /* ignora anúncios do próprio nó */
    if (dest < 0 || dest >= MAX_DEST) return;

    int nova_dist = n + 1;
    if (rota[dest].dist == INF || nova_dist < rota[dest].dist) {
        rota[dest].dist = nova_dist;
        rota[dest].succ = atoi(vizinhos[from_nb].id);
        if (rota[dest].estado == EXPEDICAO)
            envia_route_todos(dest, rota[dest].dist);
    }
}

/* Processa COORD dest recebido do vizinho from_nb.
 * Se já estiver em coordenação, responde com UNCOORD imediatamente.
 * Se o remetente for o sucessor actual, entra em coordenação;
 * caso contrário, devolve a rota conhecida seguida de UNCOORD. */
void recebe_coord(int from_nb, int dest) {
    if (monitor_on)
        printf("[MONITOR] <- %s: COORD %02d\n", vizinhos[from_nb].id, dest);

    if (dest < 0 || dest >= MAX_DEST) return;
    int j = atoi(vizinhos[from_nb].id);

    if (rota[dest].estado == COORDENACAO) {
        envia_uncoord_a(from_nb, dest);
        return;
    }

    if (j != rota[dest].succ) {
        /* Não é o sucessor actual: envia rota + UNCOORD */
        if (rota[dest].dist != INF)
            envia_route_a(from_nb, dest, rota[dest].dist);
        envia_uncoord_a(from_nb, dest);
    } else {
        /* É o sucessor actual: entra em coordenação */
        rota[dest].estado     = COORDENACAO;
        rota[dest].succ_coord = rota[dest].succ;
        rota[dest].dist       = INF;
        rota[dest].succ       = -1;
        for (int k = 0; k < nb_count; k++) {
            rota[dest].coord[k] = 1;
            envia_coord_a(k, dest);
        }
    }
}

/* Processa UNCOORD dest recebido do vizinho from_nb.
 * Quando todos os vizinhos responderam (coord[] todo a 0), volta a
 * EXPEDICAO, propaga a melhor rota conhecida e, se havia um succ_coord,
 * envia-lhe UNCOORD. */
void recebe_uncoord(int from_nb, int dest) {
    if (monitor_on)
        printf("[MONITOR] <- %s: UNCOORD %02d\n", vizinhos[from_nb].id, dest);

    if (dest < 0 || dest >= MAX_DEST) return;

    if (rota[dest].estado == COORDENACAO) {
        rota[dest].coord[from_nb] = 0;

        int todos_zero = 1;
        for (int k = 0; k < nb_count; k++)
            if (rota[dest].coord[k]) { todos_zero = 0; break; }

        if (todos_zero) {
            rota[dest].estado = EXPEDICAO;
            if (rota[dest].dist != INF)
                envia_route_todos(dest, rota[dest].dist);
            if (rota[dest].succ_coord != -1) {
                char sc_str[4];
                snprintf(sc_str, sizeof sc_str, "%02d", rota[dest].succ_coord);
                int idx = vizinho_por_id(sc_str);
                if (idx >= 0)
                    envia_uncoord_a(idx, dest);
            }
        }
    }
}
