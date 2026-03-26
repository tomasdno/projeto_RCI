#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

//////////////////////////////////////////////////////////////////
/*  Parametros do servidor nós (por omissão)                    */
/*  Aplicação invocada pelo comando:                            */
/*            OWR -  (OverlayWithRouting)                       */
/*            IP  -  endereço da máquina que aloja a aplicação  */
/*            TCP  -  porto TCP servidor da aplicação           */
/*            regIP  -  endereço IP (servidor nós)              */
/*            regUDP  -  porto UDP (servidor nós)               */
//////////////////////////////////////////////////////////////////
#define endereço_IP "193.136.138.142"
#define porto_UDP 59000

//////////////////////////////////////////////
/*          Variaveis importantes           */
//////////////////////////////////////////////
int comprimento_caminho; // numero de arestas



int main(void){
printf("Hellow World");
};

