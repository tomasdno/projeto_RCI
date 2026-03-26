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


/* 
    DESCRICAO DO FICHEIRO:
    Processamento de mensagens recebidas de vizinhos TCP, incluindo:
        - Processamento de mensagens NEIGHBOR, ROUTE, COORD, UNCOORD e CHAT
        - Leitura de dados de um fd TCP e processamento linha a linha, lidando com mensagens TCP fragmentadas
    

*/


// funções externas 
extern void recebe_route(int from_nb, int dest, int n);
extern void recebe_coord(int from_nb, int dest);
extern void recebe_uncoord(int from_nb, int dest);
extern int tcp_envia(int fd, const char *msg);
extern int vizinho_por_id(const char *id);
extern int vizinho_por_fd(int fd);

/* ================================================================
 * PROCESSAMENTO DE MENSAGENS RECEBIDAS DE UM VIZINHO TCP
 * ================================================================ */
void processa_msg_vizinho(int nb_idx, const char *linha) {
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
int le_vizinho(int fd) {
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