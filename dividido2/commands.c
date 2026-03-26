/* 
    Tomás Oliveira ist1109254, Pedro Condesso ist1110147, LEEC-25/26
    
 *  DESCRICAO DO FICHEIRO:
 *  Implementação de todos os comandos de linha de comandos do nó OWR:
 *   join, leave, exit
 *   show nodes (n), add edge (ae), remove edge (re),
 *     show neighbors (sg), direct join (dj), direct add edge (dae)
 *   announce (a), show routing (sr),
 *     start monitor (sm), end monitor (em), message (m)
 * 
 *  Inclui também processa_stdin(), que lê uma linha do stdin, faz o
 *  parse do comando e despacha para a função correspondente.
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
extern int  udp_transacao(const char *pedido, char *resposta, int resp_size);
extern int  tcp_liga(const char *ip, const char *porto);
extern int  tcp_envia(int fd, const char *msg);
extern void adiciona_vizinho(int fd, const char *id, const char *ip, const char *tcp_port);
extern void remove_vizinho(int idx);
extern void rota_init(void);
extern void envia_route_todos(int dest, int dist);
extern int  vizinho_por_id(const char *id);


/* ================================================================
 * COMANDOS join / leave
 * ================================================================ */

/* Regista o nó na rede 'net' com identificador 'id'.
 * Se directo=1 (comando dj), salta o registo no servidor de nós. */
void cmd_join(const char *net, const char *id, int directo) {
    if (joined) {
        printf("Já está na rede %s com id %s. Use 'leave' primeiro.\n",
               my_net, my_id);
        return;
    }
    strncpy(my_net, net, sizeof my_net - 1);
    strncpy(my_id,  id,  sizeof my_id  - 1);
    joined = 1;
    rota_init();

    if (directo) {
        printf("Direct join: rede=%s id=%s (sem registo no servidor)\n",
               my_net, my_id);
        return;
    }

    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "REG %03d 0 %s %s %s %s\n", tid, my_net, my_id, my_ip, my_tcp);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) {
        int rtid, rop;
        /* rop=1 -> sucesso; rop=2 -> base de dados cheia */
        if (sscanf(resposta, "REG %d %d", &rtid, &rop) == 2) {
            if (rop == 1)
                printf("Registado na rede %s com id %s\n", my_net, my_id);
            else if (rop == 2)
                printf("Erro: base de dados cheia\n");
            else
                printf("Erro no registo (op=%d)\n", rop);
        } else {
            printf("Resposta inesperada: %s\n", resposta);
        }
    }
}

/* Remove o nó da rede: fecha todas as arestas e envia pedido de
 * remoção (REG op=3) ao servidor de nós. */
void cmd_leave(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }

    while (nb_count > 0)
        remove_vizinho(0);

    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "REG %03d 3 %s %s\n", tid, my_net, my_id);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) {
        int rtid, rop;
        /* rop=4 -> remoção bem sucedida */
        if (sscanf(resposta, "REG %d %d", &rtid, &rop) == 2) {
            if (rop == 4)
                printf("Registo removido da rede %s\n", my_net);
            else
                printf("Resposta ao leave (op=%d)\n", rop);
        }
    }

    joined = 0;
    my_net[0] = '\0';
    my_id[0]  = '\0';
    printf("Saiu da rede.\n");
}


/* ================================================================
 * COMANDOS show nodes, add edge, remove edge,
 *          show neighbors, direct add edge
 * ================================================================ */

/* Lista os nós registados na rede 'net' junto do servidor de nós. */
void cmd_show_nodes(const char *net) {
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido, "NODES %03d 0 %s\n", tid, net);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return;

    int rtid, rop;
    char rnet[4];
    char *linha = resposta;
    char *nl = strchr(linha, '\n');
    if (!nl) { printf("Resposta mal formada\n"); return; }
    *nl = '\0';
    /* rop=1 -> sucesso */
    if (sscanf(linha, "NODES %d %d %3s", &rtid, &rop, rnet) != 3 || rop != 1) {
        printf("Erro ao obter nós da rede %s\n", net);
        return;
    }
    printf("Nós na rede %s:\n", net);
    linha = nl + 1;
    while (*linha) {
        nl = strchr(linha, '\n');
        if (nl) *nl = '\0';
        if (strlen(linha) > 0)
            printf("  %s\n", linha);
        if (!nl) break;
        linha = nl + 1;
    }
}

/* Obtém o contacto do nó 'id' via servidor de nós, estabelece a
 * ligação TCP e troca mensagens NEIGHBOR. */
void cmd_add_edge(const char *id) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) {
        printf("Já existe aresta com %s\n", id); return;
    }

    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "CONTACT %03d 0 %s %s\n", tid, my_net, id);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return;

    int rtid, rop;
    char rnet[4], rid[4], rip[64], rtcp[16];
    if (sscanf(resposta, "CONTACT %d %d %3s %3s %63s %15s",
               &rtid, &rop, rnet, rid, rip, rtcp) < 4) {
        printf("Resposta mal formada\n"); return;
    }
    if (rop == 2) {
        printf("Nó %s não está registado na rede %s\n", id, my_net); return;
    }
    if (rop != 1) {
        printf("Erro ao obter contacto (op=%d)\n", rop); return;
    }

    int fd = tcp_liga(rip, rtcp);
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", rip, rtcp); return; }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) {
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, rip, rtcp);
}

/* Adiciona aresta directamente, sem passar pelo servidor de nós.
 * Útil quando se conhecem previamente o IP e porto TCP do vizinho. */
void cmd_direct_add_edge(const char *id, const char *ip,
                         const char *tcp_port) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) {
        printf("Já existe aresta com %s\n", id); return;
    }

    int fd = tcp_liga(ip, tcp_port);
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", ip, tcp_port); return; }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) {
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, ip, tcp_port);
}

/* Remove a aresta para o vizinho com o id indicado. */
void cmd_remove_edge(const char *id) {
    int idx = vizinho_por_id(id);
    if (idx < 0) { printf("Não existe aresta com %s\n", id); return; }
    remove_vizinho(idx);
}

/* Lista todos os vizinhos actualmente ligados. */
void cmd_show_neighbors(void) {
    if (nb_count == 0) { printf("Sem vizinhos.\n"); return; }
    printf("Vizinhos:\n");
    for (int i = 0; i < nb_count; i++)
        printf("  id=%-3s  ip=%-20s  tcp=%s\n",
               vizinhos[i].id, vizinhos[i].ip, vizinhos[i].tcp);
}


/* ================================================================
 * COMANDOS announce, show routing, monitor, message
 * ================================================================ */

/* Anuncia o próprio nó a todos os vizinhos enviando ROUTE id 0. */
void cmd_announce(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    int id_int = atoi(my_id);
    rota[id_int].dist = 0;
    rota[id_int].succ = id_int;
    envia_route_todos(id_int, 0);
    printf("Anúncio enviado (sou %s)\n", my_id);
}

/* Mostra o estado da rota para o destino indicado. */
void cmd_show_routing(const char *dest_str) {
    int dest = atoi(dest_str);
    if (dest < 0 || dest >= MAX_DEST) { printf("Destino inválido\n"); return; }
    if (rota[dest].estado == COORDENACAO) {
        printf("Destino %02d: estado=COORDENACAO\n", dest);
    } else {
        if (rota[dest].dist == INF)
            printf("Destino %02d: estado=EXPEDICAO  dist=INF  succ=NENHUM\n", dest);
        else
            printf("Destino %02d: estado=EXPEDICAO  dist=%d  succ=%02d\n",
                   dest, rota[dest].dist, rota[dest].succ);
    }
}

/* Activa a impressão de todas as mensagens ROUTE/COORD/UNCOORD/CHAT. */
void cmd_start_monitor(void) { monitor_on = 1; printf("Monitor ON\n"); }

/* Desactiva o monitor de mensagens. */
void cmd_end_monitor(void)   { monitor_on = 0; printf("Monitor OFF\n"); }

/* Envia uma mensagem CHAT para 'dest' usando a rota de expedição actual.
 * Se o destino for o próprio nó, entrega localmente. */
void cmd_send_message(int dest, const char *texto) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (dest < 0 || dest >= MAX_DEST) { printf("Destino inválido\n"); return; }
    if (texto == NULL || *texto == '\0') { printf("Uso: message dest texto\n"); return; }

    int my_id_int = atoi(my_id);
    char texto_limpo[CHAT_MAX + 1];
    snprintf(texto_limpo, sizeof texto_limpo, "%.*s", CHAT_MAX, texto);

    if (dest == my_id_int) {
        printf("[CHAT] De %02d: %s\n", my_id_int, texto_limpo);
        return;
    }

    if (rota[dest].estado != EXPEDICAO || rota[dest].dist == INF || rota[dest].succ < 0) {
        printf("Erro: sem rota para %02d\n", dest);
        return;
    }

    char succ_str[4];
    snprintf(succ_str, sizeof succ_str, "%02d", rota[dest].succ);
    int succ_idx = vizinho_por_id(succ_str);
    if (succ_idx < 0) {
        printf("Erro: sucessor %02d não está ligado\n", rota[dest].succ);
        return;
    }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "CHAT %02d %02d %s\n", my_id_int, dest, texto_limpo);
    if (monitor_on)
        printf("[MONITOR] -> %s: %s", vizinhos[succ_idx].id, msg);

    if (tcp_envia(vizinhos[succ_idx].fd, msg) == -1) {
        printf("Erro ao enviar CHAT para %02d\n", dest);
        return;
    }
    printf("CHAT enviado para %02d via %02d\n", dest, rota[dest].succ);
}


/* ================================================================
 * PROCESSAMENTO DE COMANDOS DO UTILIZADOR (stdin)
 * ================================================================ */

/* Lê uma linha do stdin, faz o parse do comando e despacha para a
 * função correspondente.  Em caso de EOF sai do programa. */
void processa_stdin(void) {
    char linha[BUF_SIZE];
    if (!fgets(linha, sizeof linha, stdin)) {
        if (joined) cmd_leave();
        exit(0);
    }
    linha[strcspn(linha, "\n")] = '\0';
    if (strlen(linha) == 0) return;

    char cmd[32], a1[128], a2[128], a3[BUF_SIZE];
    a1[0] = a2[0] = a3[0] = '\0'; /*inicializacao dos argumentos como strings vazias */
    /* cmd -> comando; a1 -> id; a2 -> ip; a3 -> resto da linha, inicializacao dos argumentos como strongs vazias*/
    sscanf(linha, "%31s %127s %127s %1023[^\n]", cmd, a1, a2, a3);

    
    if (strcmp(cmd, "join") == 0 || strcmp(cmd, "j") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0)
            printf("Uso: join net id\n");
        else
            cmd_join(a1, a2, 0);
    }
    else if (strcmp(cmd, "leave") == 0 || strcmp(cmd, "l") == 0) {
        cmd_leave();
    }
    else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "x") == 0) {
        if (joined) cmd_leave();
        exit(0);
    }
   
    else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "show") == 0) {
        if (strcmp(cmd, "show") == 0 && strcmp(a1, "nodes") == 0)
            cmd_show_nodes(a2);
        else if (strcmp(cmd, "n") == 0)
            cmd_show_nodes(a1);
        else
            printf("Comando desconhecido\n");
    }
    else if (strcmp(cmd, "ae") == 0) {
        if (strlen(a1) == 0) printf("Uso: ae id\n");
        else cmd_add_edge(a1);
    }
    else if (strcmp(cmd, "re") == 0) {
        if (strlen(a1) == 0) printf("Uso: re id\n");
        else cmd_remove_edge(a1);
    }
    else if (strcmp(cmd, "sg") == 0) {
        cmd_show_neighbors();
    }
    else if (strcmp(cmd, "dj") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0)
            printf("Uso: dj net id\n");
        else
            cmd_join(a1, a2, 1);
    }
    else if (strcmp(cmd, "dae") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0 || strlen(a3) == 0)
            printf("Uso: dae id idIP idTCP\n");
        else
            cmd_direct_add_edge(a1, a2, a3);
    }
    
    else if (strcmp(cmd, "a") == 0) {
        cmd_announce();
    }
    else if (strcmp(cmd, "sr") == 0) {
        if (strlen(a1) == 0) printf("Uso: sr dest\n");
        else cmd_show_routing(a1);
    }
    else if (strcmp(cmd, "sm") == 0) {
        cmd_start_monitor();
    }
    else if (strcmp(cmd, "em") == 0) {
        cmd_end_monitor();
    }
    else if (strcmp(cmd, "message") == 0 || strcmp(cmd, "m") == 0) {
        if (strlen(a1) == 0 || strlen(a2) == 0) {
            printf("Uso: message dest texto\n");
        } else {
            int dest = atoi(a1);
            char texto[CHAT_MAX + 1];
            snprintf(texto, sizeof texto, "%s", a2);
            if (strlen(a3) > 0) {
                strncat(texto, " ", sizeof texto - strlen(texto) - 1);
                strncat(texto, a3, sizeof texto - strlen(texto) - 1);
            }
            cmd_send_message(dest, texto);
        }
    }
    
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
        printf("Comandos disponíveis:\n");
        printf("  j/join net id         -- entrar na rede\n");
        printf("  l/leave               -- sair da rede\n");
        printf("  x/exit                -- fechar aplicação\n");
        printf("  n net                 -- listar nós da rede\n");
        printf("  ae id                 -- adicionar aresta\n");
        printf("  re id                 -- remover aresta\n");
        printf("  sg                    -- mostrar vizinhos\n");
        printf("  dj net id             -- entrar (sem servidor)\n");
        printf("  dae id ip tcp         -- add aresta (sem servidor)\n");
        printf("  a                     -- anunciar nó\n");
        printf("  sr dest               -- mostrar rota para dest\n");
        printf("  sm                    -- activar monitor\n");
        printf("  em                    -- desactivar monitor\n");
        printf("  m/message dest texto  -- enviar mensagem para um nó\n");
    }
    else {
        printf("Comando desconhecido: '%s'. Digite 'help'.\n", cmd);
    }
}
