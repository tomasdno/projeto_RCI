/*
 * DESCRICAO DO FICHEIRO:
 * Recepção e processamento de mensagens TCP enviadas por vizinhos, incluindo:
 *   - NEIGHBOR: identificação do vizinho após ligar.
 *   - ROUTE / COORD / UNCOORD: mensagens do protocolo de encaminhamento.
 *   - CHAT: reencaminha ou entrega mensagens de chat conforme o destino.
 *   - Leitura incremental do buffer TCP, tratando mensagens fragmentadas.
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
extern void recebe_route(int from_nb, int dest, int n);
extern void recebe_coord(int from_nb, int dest);
extern void recebe_uncoord(int from_nb, int dest);
extern int  tcp_envia(int fd, const char *msg);
extern int  vizinho_por_id(const char *id);
extern int  vizinho_por_fd(int fd);


/* ================================================================
 * PROCESSAMENTO DE UMA MENSAGEM RECEBIDA DE UM VIZINHO
 * ================================================================ */

/* Despacha a mensagem 'linha' (já sem '\n') recebida do vizinho nb_idx
 * para o handler específico de cada tipo de mensagem. */
void processa_msg_vizinho(int nb_idx, const char *linha) {
    char tipo[32];
    if (sscanf(linha, "%31s", tipo) != 1) return;

    if (strcmp(tipo, "NEIGHBOR") == 0) {
        char id[4];
        if (sscanf(linha, "NEIGHBOR %3s", id) != 1) return;

        /* valida formato: exactamente 2 dígitos */
        if (strlen(id) != 2 || id[0] < '0' || id[0] > '9' ||
                                id[1] < '0' || id[1] > '9') {
            fprintf(stderr, "NEIGHBOR: id inválido '%s', a fechar ligação\n", id);
            /* fecha esta ligação para não ficar com um vizinho lixo */
            close(vizinhos[nb_idx].fd);
            /* remove_vizinho será chamado pelo ciclo principal quando
             * le_vizinho() devolver -1 — não precisamos de o chamar aqui */
            return;
        }

        /* self-loop: ignora ligações do próprio nó */
        if (strcmp(id, my_id) == 0) {
            fprintf(stderr, "NEIGHBOR: ligação do próprio nó (%s), a fechar\n", id);
            close(vizinhos[nb_idx].fd);
            return;
        }

        /* duplicado: já existe vizinho com este id num fd diferente */
        int dup = vizinho_por_id(id);
        if (dup >= 0 && dup != nb_idx) {
            fprintf(stderr, "NEIGHBOR: id %s já existe (fd=%d), a ignorar\n",
                    id, vizinhos[dup].fd);
            close(vizinhos[nb_idx].fd);
            return;
        }

        /* actualiza o id do vizinho */
        char old_id[4];
        strncpy(old_id, vizinhos[nb_idx].id, sizeof old_id - 1);
        old_id[sizeof old_id - 1] = '\0';
        strncpy(vizinhos[nb_idx].id, id, sizeof vizinhos[nb_idx].id - 1);
        vizinhos[nb_idx].id[sizeof vizinhos[nb_idx].id - 1] = '\0';
        if (strcmp(old_id, id) != 0) {
            if (strcmp(old_id, "??") == 0)
                printf("Vizinho %s adicionado (fd=%d)\n", id, vizinhos[nb_idx].fd);
            else
                printf("Vizinho %s identificado como %s (fd=%d)\n",
                       old_id, id, vizinhos[nb_idx].fd);
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
        int origin, dest;
        char mensagem[CHAT_MAX + 1] = "";
        if (sscanf(linha, "CHAT %d %d %128[^\n]", &origin, &dest, mensagem) >= 2) {
            /* validação de bounds antes de qualquer acesso a rota[] */
            if (origin < 0 || origin >= MAX_DEST || dest < 0 || dest >= MAX_DEST) {
                fprintf(stderr, "CHAT: origin/dest fora do intervalo (%d->%d)\n", origin, dest);
                return;
            }
            int my_id_int = atoi(my_id);
            if (dest == my_id_int) {
                printf("[CHAT] De %02d: %s\n", origin, mensagem);
            } else {
                if (rota[dest].dist != INF && rota[dest].succ >= 0) {
                    char succ_str[4];
                    snprintf(succ_str, sizeof succ_str, "%02d", rota[dest].succ);
                    int succ_idx = vizinho_por_id(succ_str);
                    if (succ_idx >= 0) {
                        char fwd[BUF_SIZE];
                        snprintf(fwd, sizeof fwd, "CHAT %02d %02d %s\n", origin, dest, mensagem);
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


/* ================================================================
 * LEITURA INCREMENTAL DO BUFFER TCP
 * ================================================================ */

/* Lê dados disponíveis no fd TCP para o buffer parcial associado e
 * processa todas as linhas completas (terminadas em '\n').
 * Os dados incompletos ficam no buffer à espera do próximo read().
 * Devolve 0 em caso de sucesso, -1 se a ligação foi fechada ou houve erro. */
int le_vizinho(int fd) {
    if (fd < 0 || fd >= 1024) return -1;
    ReadBuf *b = &rb[fd];
    ssize_t n = read(fd, b->buf + b->len, sizeof(b->buf) - b->len - 1);
    if (n <= 0) return -1;
    b->len += (int)n;
    b->buf[b->len] = '\0';

    char *ptr = b->buf;
    char *nl;
    while ((nl = memchr(ptr, '\n', b->buf + b->len - ptr)) != NULL) {
        *nl = '\0';
        int nb_idx = vizinho_por_fd(fd);
        if (nb_idx >= 0)
            processa_msg_vizinho(nb_idx, ptr);
        ptr = nl + 1;
    }

    /* Move dados incompletos para o início do buffer */
    int restante = (int)(b->buf + b->len - ptr);
    memmove(b->buf, ptr, restante);
    b->len = restante;
    return 0;
}
