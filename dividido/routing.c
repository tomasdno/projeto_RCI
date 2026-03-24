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

/* Extern declarations */
extern int tcp_envia(int fd, const char *msg);
extern int vizinho_por_id(const char *id);

/* ================================================================
 * INICIALIZAÇÃO DE ENCAMINHAMENTO
 * ================================================================ */
void rota_init(void) {
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
 * MENSAGENS DE ENCAMINHAMENTO – ENVIO
 * ================================================================ */
void envia_route_todos(int dest, int dist) { //msg de rotta para todos os vizinhos com destino dest, desitancia dist 
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist); //msg de rota para o destino dest com distância dist
    for (int i = 0; i < nb_count; i++) { // envia a mensagem de rota para todos os vizinhos conectados
        if (monitor_on)
            printf("[MONITOR] -> %s: %s", vizinhos[i].id, msg);
        tcp_envia(vizinhos[i].fd, msg); //msg rota para vizinho i com destino dest e distância dist
    }
}

void envia_route_a(int nb_idx, int dest, int dist) { // msg para vizinho nb_idx com destino dest e distância dist
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "ROUTE %02d %d\n", dest, dist); // msg de rota para o vizinho nb_idx com destino dest e distância dist
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg); //imprime msg rota caso monitor esteja ativo
    tcp_envia(vizinhos[nb_idx].fd, msg); //msg de rota para vizinho[nb_idx] com destino dest e distância dist
}

void envia_coord_a(int nb_idx, int dest) { // msg para vizinho nb_idx com destino dest 
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "COORD %02d\n", dest); // msg coordenação destino dest
    if (monitor_on) 
        printf("[MONITOR] -> %s: %s", vizinhos[nb_idx].id, msg); 
    tcp_envia(vizinhos[nb_idx].fd, msg); // envia a msg de coordenação para o vizinho nb_idx
}

void envia_uncoord_a(int nb_idx, int dest) {
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
void recebe_route(int from_nb, int dest, int n) { //msg rota, destino dest, distancia n, recebida do vizinho from_nb
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
void recebe_coord(int from_nb, int dest) {
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
void recebe_uncoord(int from_nb, int dest) {
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