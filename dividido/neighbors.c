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
extern void envia_route_a(int nb_idx, int dest, int dist);
extern void envia_coord_a(int nb_idx, int dest);
extern void envia_uncoord_a(int nb_idx, int dest);

/* Encontra vizinho pelo fd */
int vizinho_por_fd(int fd) { // percorre a lista de vizinhos para encontrar o índice do vizinho com o fd especificado
    for (int i = 0; i < nb_count; i++)  
        if (vizinhos[i].fd == fd) return i; // se encontrar o vizinho com o fd correspondente, retorna o índice
    return -1;
}

/* Encontra vizinho pelo id */
int vizinho_por_id(const char *id) {
    for (int i = 0; i < nb_count; i++)
        if (strcmp(vizinhos[i].id, id) == 0) return i;
    return -1;
}

/* Adiciona vizinho já com fd e id conhecidos */
void adiciona_vizinho(int fd, const char *id,
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
void remove_vizinho(int idx) {
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