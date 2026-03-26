/* 
    Tomás Oliveira ist1109254, Pedro Condesso ist1110147, LEEC-25/26

 * DESCRICAO DO FICHEIRO:
 * Gestão da lista de vizinhos TCP activos, incluindo:
 *   - Pesquisa de vizinho por fd ou por id.
 *   - Adição de novo vizinho, com envio imediato das rotas conhecidas.
 *   - Remoção de vizinho, actualizando a tabela de roteamento e iniciando
 *     coordenação para os destinos cujo sucessor era o vizinho removido.
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
extern void envia_route_a(int nb_idx, int dest, int dist);
extern void envia_coord_a(int nb_idx, int dest);
extern void envia_uncoord_a(int nb_idx, int dest);
extern int  tcp_envia(int fd, const char *msg);


/* ================================================================
 * PESQUISA DE VIZINHOS
 * ================================================================ */

/* Devolve o índice do vizinho com o fd indicado, ou -1 se não existir. */
int vizinho_por_fd(int fd) {
    for (int i = 0; i < nb_count; i++)
        if (vizinhos[i].fd == fd) return i;
    return -1;
}

/* Devolve o índice do vizinho com o id indicado, ou -1 se não existir. */
int vizinho_por_id(const char *id) {
    for (int i = 0; i < nb_count; i++)
        if (strcmp(vizinhos[i].id, id) == 0) return i;
    return -1;
}


/* ================================================================
 * ADIÇÃO DE VIZINHO
 * ================================================================ */

/* Regista um novo vizinho TCP na lista global.
 * Se já houver rotas em estado EXPEDICAO, envia-lhes ROUTE imediatamente
 * para que o novo vizinho fique a par do estado da rede. */
void adiciona_vizinho(int fd, const char *id,
                      const char *ip, const char *tcp_port) {
    if (nb_count >= MAX_VIZINHOS) {
        fprintf(stderr, "Erro: número máximo de vizinhos atingido\n");
        close(fd);
        return;
    }
    int i = nb_count++;
    vizinhos[i].fd = fd;
    strncpy(vizinhos[i].id,  id,       sizeof vizinhos[i].id  - 1);
    strncpy(vizinhos[i].ip,  ip,       sizeof vizinhos[i].ip  - 1);
    strncpy(vizinhos[i].tcp, tcp_port, sizeof vizinhos[i].tcp - 1);
    memset(&rb[fd], 0, sizeof rb[fd]);

    for (int d = 0; d < MAX_DEST; d++) {
        rota[d].coord[i] = 0;  /* por omissão não é dependência de coordenação */
        if (rota[d].estado == EXPEDICAO && rota[d].dist != INF)
            envia_route_a(i, d, rota[d].dist);
    }

    if (strcmp(id, "??") != 0)
        printf("Vizinho %s adicionado (fd=%d)\n", id, fd);
}


/* ================================================================
 * REMOÇÃO DE VIZINHO
 * ================================================================ */

/* Remove o vizinho no índice idx, fecha a ligação TCP e actualiza a
 * tabela de roteamento.  Para cada destino em que o vizinho removido
 * era o sucessor, inicia o protocolo de coordenação com os restantes
 * vizinhos. */
void remove_vizinho(int idx) {
    int dest_fd = vizinhos[idx].fd;
    printf("Vizinho %s removido\n", vizinhos[idx].id);
    close(dest_fd);

    /* Protocolo de encaminhamento: remoção de aresta */
    // só actualiza rotas se o vizinho estava identificado 
    int vizinho_id_int = -1;
    if (strcmp(vizinhos[idx].id, "??") != 0)
        vizinho_id_int = atoi(vizinhos[idx].id);

    for (int d = 0; d < MAX_DEST; d++) {
        if (rota[d].estado == EXPEDICAO) {
            /* só entra em coordenação se o sucessor era este vizinho identificado */
            if (vizinho_id_int >= 0 && rota[d].succ == vizinho_id_int) {
                rota[d].estado     = COORDENACAO;
                rota[d].succ_coord = -1;
                rota[d].dist       = INF;
                rota[d].succ       = -1;
                for (int k = 0; k < nb_count; k++) {
                    if (k == idx) continue;
                    rota[d].coord[k] = 1;
                    envia_coord_a(k, d);
                }
                /* verifica se há outros vizinhos dependentes dessa coordenação, para decidir se mantém o estado de coordenação ou volta a expedição */
                int tem_outros = 0;
                for (int k = 0; k < nb_count; k++)
                    if (k != idx && rota[d].coord[k]) { tem_outros = 1; break; }
                if (!tem_outros) rota[d].estado = EXPEDICAO;
            }
        } else {
            /* COORDENACAO: vizinho removido já não é dependência */
            rota[d].coord[idx] = 0;
            /* ver se algum vizinho tem coord = 1 para este dest */
            int todos_zero = 1;
            for (int k = 0; k < nb_count; k++)
                if (k != idx && rota[d].coord[k]) { todos_zero = 0; break; }
            /* se nao houver dependencias, volta a por estado expedicao */
            if (todos_zero) {
                rota[d].estado = EXPEDICAO;
                if (rota[d].dist != INF) {
                    char msg_r[BUF_SIZE];
                    snprintf(msg_r, sizeof msg_r, "ROUTE %02d %d\n", d, rota[d].dist);
                    for (int k = 0; k < nb_count; k++) {
                        if (k == idx) continue;
                        if (monitor_on)
                            printf("[MONITOR] -> %s: %s", vizinhos[k].id, msg_r);
                        tcp_envia(vizinhos[k].fd, msg_r);
                    }
                }
                /* envio uncoord para sucessor */
                if (rota[d].succ_coord != -1) {
                    char sc_str[4];
                    snprintf(sc_str, sizeof sc_str, "%02d", rota[d].succ_coord);
                    int sc_idx = vizinho_por_id(sc_str);
                    if (sc_idx >= 0 && sc_idx != idx)
                        envia_uncoord_a(sc_idx, d);
                }
            }
        }
    }

    /* swap com o último e corrige o array coord */
    memset(&rb[dest_fd], 0, sizeof rb[dest_fd]);
    int last = --nb_count;
    if (idx != last) {
        vizinhos[idx] = vizinhos[last];
        /* move o coord do vizinho que foi para a posição idx */
        for (int d = 0; d < MAX_DEST; d++) {
            rota[d].coord[idx]  = rota[d].coord[last];
            rota[d].coord[last] = 0;
        }
    } else {
        /* o removido era o último: limpa coord */
        for (int d = 0; d < MAX_DEST; d++)
            rota[d].coord[last] = 0;
    }
}
