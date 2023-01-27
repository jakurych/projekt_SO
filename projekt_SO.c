#include <sys/ipc.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

//S1: SIGTERM
//S2: SIGINT
//S3: SIGCONT



#define sleep_default usleep(250000) //usleep w ms sam sleep w sec

#define OUTPUT stderr 
key_t ipc_key; //klucz kolejki  
int queue_id; 

pid_t* pids_shared;  

//id procesow potrzebne do rozprowadzania sygnalow
pid_t c_pid1 = -1; 
pid_t c_pid2 = -1;
pid_t c_pid3 = -1;

int process_type = 0;

int start_work = 0;

int main_send_next = 0;

typedef struct buffer_message
{
	long mtype;
	char data[1024];
} msg_t;



void decToHex(char *tabS, char *tabH, int n_max) { //konwersja na hex
    int j=0, i=0;

    while(j < n_max) {
        sprintf(tabH+i,"%02X", (int)tabS[j]);
        j+=1;
        i+=2;
    }
    tabH[i++] = '\0';
}

void* new_shared_mem(size_t size){ 
    return mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
}

struct buffer_message receive_message(){
    struct buffer_message nmsg;	
	int t =  msgrcv(queue_id, &nmsg, sizeof(nmsg.data), 0, 0);

    return nmsg;
}


void await_parameters(){ 
    while (start_work == 0){
        usleep(1000);
    }
    c_pid1 = pids_shared[0];
    c_pid2 = pids_shared[1];
    c_pid3 = pids_shared[2];
    fprintf(stderr, "Received into thread %i, p1:%i, p2:%i, p3:%i\n", process_type, c_pid1, c_pid2, c_pid3);

}

void signal_handler(int signal){ 
    if (signal == SIGUSR2){
        start_work++;
    }
}

int proc2_receive_next = 0;
int proc2_write_file = 1;

void signal_handler_proc2(int signal){
    if (signal == SIGUSR1){
        proc2_receive_next++;
    } else if (signal == SIGUSR2){
        proc2_write_file = 1; 
    }
}

int proc3_receive_next = 0;

void signal_handler_proc3(int signal){
    if (signal == SIGUSR1){
        proc3_receive_next++;
    }
}

int globalmod_paused = 0; 


void signal_global(int signal){
    if (signal == SIGTERM || signal == SIGCONT || signal == SIGINT){
        pid_t pids[] = {c_pid1, c_pid2, c_pid3}; 
        for (int i = 0; i < 3; i++){
            if (pids[i] != -1 && pids[i] != getpid()){
                kill(pids[i], SIGRTMIN+signal); 
            }
        }
    }
    int action = signal;
    if (action > SIGRTMIN){
        action -= SIGRTMIN;
    }

    switch (action){
        case SIGTERM:
            kill(getpid(), SIGKILL);
            break;
        case SIGCONT:
            globalmod_paused = 0; //do odpauzowania 
            break;
        case SIGINT:
            globalmod_paused = 1; //do zatrzymywania 
            break;
    }

}


void signal_handler_main(int signal){ 
    if (signal == SIGUSR1){
        main_send_next = 1;
    }
    signal_global(signal);
}


void register_globals(){
    signal(SIGTERM, signal_global);
    signal(SIGCONT, signal_global);
    signal(SIGINT, signal_global);
    signal(SIGRTMIN+SIGTERM, signal_global);
    signal(SIGRTMIN+SIGCONT, signal_global);
    signal(SIGRTMIN+SIGINT, signal_global);
}

void proc1(char* target){
    register_globals();
    signal(SIGUSR2, signal_handler);
    //printf("proc1 start\n");
    await_parameters();
    sleep(2);
    
    char buffer[1024];
    struct buffer_message bfr_msg;
    if (strcmp(target, "/dev/stdin") == 0){
        printf("Input: ");
        while (1){
            if (!globalmod_paused){
                scanf("%s", buffer);
                bfr_msg.mtype = strlen(buffer); //dlugosc stringa ktorego wpisujemy 
                memcpy(bfr_msg.data, buffer, bfr_msg.mtype > 1024 ? 1024 : bfr_msg.mtype);
                msgsnd(queue_id, &bfr_msg, sizeof(bfr_msg.data), 0);
                kill(c_pid2, SIGUSR1); //wyslanie sygnalu SIGUR1 do procesu 2 
            }
        }
    } else {
        FILE* f = fopen(target, "rb");
        while (!feof(f)){
            int d = fread(buffer, 1, 1024, f); //wczytanie do bufora 1 raz po 1024 bajty ww pliku f 
            bfr_msg.mtype = d; //na ilosc bajtow 
            memcpy(bfr_msg.data, buffer, d); //kopiowanie z bufora do danych 
            msgsnd(queue_id, &bfr_msg, sizeof(bfr_msg.data), 0); //wyslanie 
            kill(c_pid2, SIGUSR1); //do procesu 2 info ze zotstalo wyslane 
            if (strcmp(target, "/dev/urandom") == 0){
                sleep(1); //zeby robil co sekunde aby zdarzyz wyswietlac 
            } else {
                sleep_default; //normalny plik 
            }
        }
        fclose(f);
        raise(SIGTERM); //terminacie 
    }
    printf("proc1 exit\n");
    
}

void proc2(){
    int next = proc2_receive_next; 
    register_globals();
    signal(SIGUSR1, signal_handler_proc2); //dodaje do recivenext 
    signal(SIGUSR2, signal_handler); //sygnal z procesu 3
    //fprintf(OUTPUT, "proc2 start\n");
    await_parameters(); 
    signal(SIGUSR2, signal_handler_proc2);
    sleep(1);

    char buffer[2049];

    while(1){
    //SIGINT
        if (!globalmod_paused){
            if (next != proc2_receive_next){ //jezeli jest nowa wiadomosc 
                struct buffer_message nmsg = receive_message(); //przyjmuj ta wiadomosc 
                //printf("[2] Received message: %s ...\n", nmsg.data);
                next = proc2_receive_next; 
                
                decToHex(nmsg.data, buffer, nmsg.mtype);

                while (proc2_write_file == 0){ //czekanie na sygnal z procesu 3 ze zczytal dane i wyswietlil 
                
                }
                
                //printf("Writing file\n");
                FILE* nfile = fopen("_proc2_data.bin~", "w+"); //w+ nadpisanie 
                fwrite(buffer, 2048, 1, nfile);
                proc2_write_file = 0; 
                fclose(nfile);
                kill(c_pid3, SIGUSR1); 

            }
        }
        sleep_default;
    }
    //printf("proc2 exit\n");
}

void proc3(){
    //printf("proc3 start\n");
    int next = proc3_receive_next; 
    register_globals();
    signal(SIGUSR1, signal_handler_proc3);
    signal(SIGUSR2, signal_handler);

    await_parameters();
    
    char buffer[2048]; //2 razy wiekszy bo hex
    
    while(1){
        if (!globalmod_paused){
            if (next != proc3_receive_next){
                FILE* nfile = fopen("_proc2_data.bin~", "r");
                fread(buffer, 2048, 1, nfile);
                fclose(nfile);
                printf("[3] Output:\n");
                char* buffer2 = &(buffer[0]);
                int chars = 0;
                while (*buffer2){
                    printf("%c%c ", *buffer2, *(buffer2+1));
                    chars++;
                    if (chars == 16){
                        chars = 0;
                        printf("\n");
                    }
                    buffer2+=2;
                }
                printf("\n");
                kill(c_pid2, SIGUSR2);//procesie 2 - mozesz zapisywac kolejne 
                next = proc3_receive_next; //sprawdzenie czy sa nowe dane 
            }
        }
        sleep_default;
    }
    //printf("proc3 exit\n");
}

int main(int argc, char** argv){
    char target_file[4096] = "/dev/urandom";

    printf("Tryb dzialania: \n");
    printf("1: Tryb interaktywny\n");
    printf("2: Odczyt z pliku\n");
    printf("3: Odczyt z /dev/urandom\n");
    int wybor = -1;
    while (wybor < 1 || wybor > 3){
        scanf("%i", &wybor);
    }
    if (wybor == 2){
        printf("Podaj nazwe pliku: \n");
        scanf("%s", target_file);
        while (access(target_file, F_OK) != 0){
            printf("Plik %s nie istnieje\n", target_file);
            printf("Podaj nazwe pliku: \n");
            scanf("%s", target_file);
        }
    } else {
        strcpy(target_file, 
            wybor == 1 ? "/dev/stdin" 
            : wybor == 3 ? "/dev/urandom"
            : ""
        );       
    }

    pids_shared = new_shared_mem(sizeof(pid_t)*3); 

    ipc_key = ftok(".", 2); //klucz do kolejki
    queue_id = msgget(ipc_key, IPC_CREAT | 0660); 
    signal(SIGUSR1, signal_handler_main);
    signal(SIGKILL, signal_handler_main);
    signal(SIGTERM, signal_handler_main);
    signal(SIGINT, signal_handler_main);
    signal(SIGCONT, signal_handler_main);
    printf("Starting process 1\n");
    pid_t p1 = fork();
    
    if (p1 == 0){
        process_type = 1; 
        proc1(target_file); 
    } else {
        pids_shared[0] = p1;//zapis do tablicy pw
        printf("Starting process 2\n");
        pid_t p2 = fork();
        if (p2 == 0){
            process_type = 2;
            proc2();
        } else {
            pids_shared[1] = p2;
            printf("Starting process 3\n");
            pid_t p3 = fork(); 
            //printf("%i\n", p3);
            if (p3 == 0){
                process_type = 3;
                proc3();
            } else {
                pids_shared[2] = p3; 
                
                c_pid1 = p1;
                c_pid2 = p2;
                c_pid3 = p3;
                
                
                sleep(2);
		
		//wysylamy po to aby ruszyly do pracy aby mogly odczytywac z PW 
                kill(p1, SIGUSR2);
                kill(p2, SIGUSR2);
                kill(p3, SIGUSR2);

                int status;
                waitpid(p1, &status, 0); //czekanie az proces sie zakonczt
                printf("p1 exit code: %i\n", status);
                waitpid(p2, &status, 0);
                printf("p2 exit code: %i\n", status);
                waitpid(p3, &status, 0);
                printf("p3 exit code: %i\n", status);
			
		//kod 9 - sig kill - wszystkie zostaly zakonczone przez sugnal global przez 
		//sigterm
                msgctl(queue_id, IPC_RMID, NULL);
                fprintf(OUTPUT, "main exit\n");
            }
        }
    }
    return 0;
}
