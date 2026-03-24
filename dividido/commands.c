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
extern int udp_transacao(const char *pedido, char *resposta, int resp_size);
extern int tcp_liga(const char *ip, const char *porto);
extern int tcp_envia(int fd, const char *msg);
extern void adiciona_vizinho(int fd, const char *id, const char *ip, const char *tcp_port);
extern void remove_vizinho(int idx);
extern void rota_init(void);
extern void envia_route_todos(int dest, int dist);
extern int vizinho_por_id(const char *id);

/* ================================================================
 * COMANDOS: ETAPA 2 – join / leave
 * ================================================================ */

/* join net id  OU  direct join net id */
void cmd_join(const char *net, const char *id, int directo) {
    if (joined) { // se já estiver em uma rede, não pode se juntar a outra sem sair primeiro
        printf("Já está na rede %s com id %s. Use 'leave' primeiro.\n",
               my_net, my_id);
        return;
    }
    strncpy(my_net, net, sizeof my_net - 1);
    strncpy(my_id,  id,  sizeof my_id  - 1);
    joined = 1;
    rota_init(); //inicia tabela de roteamento para o nó, definindo a distância para si próprio como 0 e o sucessor como si próprio

    if (directo) {
        printf("Direct join: rede=%s id=%s (sem registo no servidor)\n",
               my_net, my_id);
        return;
    }

    /* REG tid 0 net id IP TCP\n */
    int tid = rand() % 1000; // gera tdi aleatório para a transação de registro
    char pedido[BUF_SIZE], resposta[BUF_SIZE]; // buffers para armazenar pedido de registo e resposta do serbior de nós
    snprintf(pedido, sizeof pedido,
             "REG %03d 0 %s %s %s %s\n", tid, my_net, my_id, my_ip, my_tcp);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) { //se udp tiver sucesso, processa a resposta do servidor de nós
        int rtid, rop; // armazenar tid de resposta e código de operação da resposta do servidor de nós
        // rop -> 1 = registo bem sucedido, 2 = erro base de dados cheia, outros valores = erro no registo
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

// leave -> remove todas as arestas, envia pedido de remoção ao servidor de nós e limpa estado local
void cmd_leave(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }

    /* Fecha todas as arestas */
    while (nb_count > 0) // passa por todos os vizinhos e remove um por um até não ter mais vizinhos
        remove_vizinho(0);

    /* REG tid 3 net id\n  (pedido de remoção) */
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "REG %03d 3 %s %s\n", tid, my_net, my_id);

    if (udp_transacao(pedido, resposta, sizeof resposta) == 0) {
        int rtid, rop;
        // rop -> 4 = remoção bem sucedida, outros valores = erro na remoção
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
 * COMANDOS: ETAPA 3 – show nodes, add edge, remove edge,
 *                     show neighbors, direct add edge
 * ================================================================ */

/* show nodes (n) net */
void cmd_show_nodes(const char *net) {
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido, "NODES %03d 0 %s\n", tid, net);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return; // se servidor de nós não responder, retorna sem processar a resposta

    /* Resposta: "NODES tid 1 net\nid1\nid2\n..." */
    int rtid, rop; // tid resposta e operacao de resposts
    char rnet[4]; // nome da rede na resposta do servidor de nós
    char *linha = resposta; // ponteiro para percorrer a resposta do servidor de nós linha a linha
    /* Lê cabeçalho */
    char *nl = strchr(linha, '\n'); // encontra \n
    if (!nl) { printf("Resposta mal formada\n"); return; }
    *nl = '\0';
    if (sscanf(linha, "NODES %d %d %3s", &rtid, &rop, rnet) != 3 || rop != 1) { //rop = 1 -> sucesso, outros valores -> erro
        printf("Erro ao obter nós da rede %s\n", net);
        return;
    }
    printf("Nós na rede %s:\n", net);
    linha = nl + 1; // atualiza o ponteiro para a próxima linha após o cabeçalho
    // iterar pela resposta do servidor de nós linha a linha, imprimindo os ids dos nós encontrados ao encontrar um \n, substitui por \0 para conseguir usar isso como string e imprime o id do nó
    while (*linha) {
        nl = strchr(linha, '\n');
        if (nl) *nl = '\0';
        if (strlen(linha) > 0)
            printf("  %s\n", linha);
        if (!nl) break;
        linha = nl + 1;
    }
}

/* add edge (ae) id  –  obtém contacto via servidor de nós e liga */
void cmd_add_edge(const char *id) { //
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) { // se já existe um vizinho com o id especificado, não pode adicionar uma nova aresta para esse id
        printf("Já existe aresta com %s\n", id); return;
    }

    /* CONTACT tid 0 net id\n */
    int tid = rand() % 1000;
    char pedido[BUF_SIZE], resposta[BUF_SIZE];
    snprintf(pedido, sizeof pedido,
             "CONTACT %03d 0 %s %s\n", tid, my_net, id);

    if (udp_transacao(pedido, resposta, sizeof resposta) != 0) return; //erro caso servidor de nos nao responda

    int rtid, rop; 
    char rnet[4], rid[4], rip[64], rtcp[16]; // rnet -> nome da rede na resposta do servidor de nós, rid -> id do nó encontrado, rip -> ip do nó encontrado, rtcp -> porto tcp do nó encontrado
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

    /* Liga por TCP */
    int fd = tcp_liga(rip, rtcp); // conexão tcp entre o nó atual e o nó encontrado com ip rip e porto rtcp
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", rip, rtcp); return; }

    /* Envia NEIGHBOR my_id */
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) { // envia mensagem de vizinhança para o nó encontrado, caso haja erro no envio, fecha a conexão tcp e retorna
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, rip, rtcp); // adiciona o no encontrado como vizinho, com o fd da conexão tcp, id do nó encontrado, ip e porto tcp do nó encontrado
}

/* direct add edge (dae) id idIP idTCP */
void cmd_direct_add_edge(const char *id, const char *ip,
                                 const char *tcp_port) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    if (vizinho_por_id(id) >= 0) { // se já existe um vizinho com o id especificado, não pode adicionar uma nova aresta para esse id
        printf("Já existe aresta com %s\n", id); return;
    }

    int fd = tcp_liga(ip, tcp_port); // conexão tcp entre o nó atual e o nó com ip e porto tcp especificados
    if (fd == -1) { printf("Erro ao ligar a %s:%s\n", ip, tcp_port); return; }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg, "NEIGHBOR %s\n", my_id);
    if (tcp_envia(fd, msg) == -1) { // envia mensagem para todos ps vizinhos com o id do nó atual, caso haja erro no envio, fecha a conexão tcp e retorna
        printf("Erro ao enviar NEIGHBOR\n"); close(fd); return;
    }

    adiciona_vizinho(fd, id, ip, tcp_port); //adiciona o nó como vizinho, com o fd da conexão tcp, id do nó, ip e porto tcp especificados
}

/* remove edge (re) id */
void cmd_remove_edge(const char *id) { // remove a aresta para o vizinho com o id especificado
    int idx = vizinho_por_id(id); // encontra o índice do vizinho com o id especificado
    if (idx < 0) { printf("Não existe aresta com %s\n", id); return; } // caso nao encontre, da erro
    remove_vizinho(idx); // remove o vizinho encontrado, que fecha a conexão tcp e atualiza as rotas para os destinos que tinham esse vizinho como sucessor, entrando em coordenação se necessário
}

/* show neighbors (sg) */
void cmd_show_neighbors(void) { 
    if (nb_count == 0) {
        printf("Sem vizinhos.\n"); return;
    }
    printf("Vizinhos:\n");
    for (int i = 0; i < nb_count; i++) //percorre lista de vizinhos que esta no vetor na porsição i = 0 até ao nº de vizinhos nb_count, imprimindo o id, ip e porto tcp de cada vizinho
        printf("  id=%-3s  ip=%-20s  tcp=%s\n",
               vizinhos[i].id, vizinhos[i].ip, vizinhos[i].tcp);
}

/* ================================================================
 * COMANDOS: ETAPA 4 – announce, show routing, monitor
 * ================================================================ */

/* announce (a) – anuncia o próprio nó a todos os vizinhos */
void cmd_announce(void) {
    if (!joined) { printf("Não está em nenhuma rede.\n"); return; }
    int id_int = atoi(my_id); // converção de string do id do no para int
    /* Distância 0 a si próprio */
    rota[id_int].dist = 0;
    rota[id_int].succ = id_int;
    /* Envia ROUTE id 0 a todos os vizinhos */
    envia_route_todos(id_int, 0); // msg de rota para vizinhos com destino id_int (o próprio nó) e distância 0
    printf("Anúncio enviado (sou %s)\n", my_id);
}

/* show routing (sr) dest */
void cmd_show_routing(const char *dest_str) {
    int dest = atoi(dest_str); // converção de string do destino para int
    if (dest < 0 || dest >= MAX_DEST) { // verificação de destino válido
        printf("Destino inválido\n"); return;
    }
    if (rota[dest].estado == COORDENACAO) { //
        printf("Destino %02d: estado=COORDENACAO\n", dest);
    } else {
        if (rota[dest].dist == INF) //se dist for inf, entao nao ha rota esse para o destino
            printf("Destino %02d: estado=EXPEDICAO  dist=INF  succ=NENHUM\n", dest);
        else
            printf("Destino %02d: estado=EXPEDICAO  dist=%d  succ=%02d\n",
                   dest, rota[dest].dist, rota[dest].succ); // se dist for diferente de inf, imprime a distância e o sucessor para esse destino
    }
}

/* start monitor / end monitor */
void cmd_start_monitor(void) { monitor_on = 1; printf("Monitor ON\n"); }
void cmd_end_monitor(void)   { monitor_on = 0; printf("Monitor OFF\n"); }

/* ================================================================
 * PROCESSAMENTO DE COMANDOS DO UTILIZADOR (stdin)
 * ================================================================ */
void processa_stdin(void) {
    char linha[BUF_SIZE]; // buffer da linha de comando
    if (!fgets(linha, sizeof linha, stdin)) { // ler a linha stdin
        /* EOF no stdin -> sai */
        if (joined) cmd_leave();
        exit(0);
    }
    /* Remove \n */
    linha[strcspn(linha, "\n")] = '\0';
    if (strlen(linha) == 0) return;

    char cmd[32], a1[128], a2[128], a3[BUF_SIZE]; // buffers para comando e argumentos
    a1[0] = a2[0] = a3[0] = '\0'; 
    sscanf(linha, "%31s %127s %127s %1023[^\n]", cmd, a1, a2, a3); // cmd->comando a1->id a2->ip a3->tcp_port 

    /* --- Etapa 2 --- */
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
    /* --- Etapa 3 --- */
    else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "show") == 0) {
        /* "show nodes net"  ou  "n net" */
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
    /* --- Etapa 4 --- */
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
    /* --- Ajuda --- */
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
    }
    else {
        printf("Comando desconhecido: '%s'. Digite 'help'.\n", cmd);
    }
}