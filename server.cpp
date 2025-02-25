#include <stdio.h>
#include <semaphore.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <locale.h>
#include <time.h>
#include <stdlib.h>
#include <ncursesw/ncurses.h>
#include "common.h"
#include "server_data.h"
#include "tiles.h"

// Szerokość i wysokość panelu z logami
#define LOG_LINES_COUNT 7
#define LOG_LINE_WIDTH 35

// O ile jednorazowo przesówa się mapa przy pciskaniu strzałem
#define MAP_SHIFT_JUMP_X 1
#define MAP_SHIFT_JUMP_Y 1

// Makro wariadyczne dodające logi
#define SERVER_ADD_LOG(__msg, __args...)\
{\
    for(int i=LOG_LINES_COUNT-1; i>0; i--)\
        strncpy(logs[i], logs[i-1], LOG_LINE_WIDTH);\
    snprintf(logs[0], LOG_LINE_WIDTH, __msg, ## __args);\
}

// Prototypy funkcji
void *server_display_thread(void *ptr);
void *server_input_thread(void *ptr);
void server_init_ncurses(void);
void server_init_sm(void);
void server_display_stats(void);
void server_display_logs(void);
void *server_update_thread(void *ptr);

// Pamięć współdzielona
int fd;
struct clients_sm_block_t *sm_block;

// Wyświetlane okna
WINDOW *stat_window;
WINDOW *log_window;
WINDOW *map_window;
WINDOW *help_window;

// Wyświetlane logi
char logs[LOG_LINES_COUNT][LOG_LINE_WIDTH+1];

// Działające wątki
pthread_t input_thread;
pthread_t update_thread;

// Wszystkie dane serwera - oddzielone od mechanizmu komunikacji
struct server_data_t server_data;


// Wątek obsługujący klawiature
void *server_input_thread(void *ptr)
{
    while(1)
    {
        int c = getch();
        if(tolower(c)=='q') 
        {
            SERVER_ADD_LOG("Confirm exiting with y/n");
            c = getchar();
            if(c=='y') return NULL;
            else SERVER_ADD_LOG("Exiting canceled");
        }
        else if(tolower(c)=='b')
        {
            pthread_mutex_lock(&server_data.update_vs_input_mutex);
            SERVER_ADD_LOG("Adding beast");
            sd_add_beast(&server_data);
            pthread_mutex_unlock(&server_data.update_vs_input_mutex);
        }
        else if(c=='c')
        {
            pthread_mutex_lock(&server_data.update_vs_input_mutex);
            SERVER_ADD_LOG("Adding coin");
            sd_add_something(&server_data, TILE_COIN);
            pthread_mutex_unlock(&server_data.update_vs_input_mutex);
        }
        else if(c=='t')
        {
            pthread_mutex_lock(&server_data.update_vs_input_mutex);
            SERVER_ADD_LOG("Adding small treasure");
            sd_add_something(&server_data, TILE_S_TREASURE);
            pthread_mutex_unlock(&server_data.update_vs_input_mutex);
        }
        else if(c=='T')
        {
            pthread_mutex_lock(&server_data.update_vs_input_mutex);
            SERVER_ADD_LOG("Adding big treasure");
            sd_add_something(&server_data, TILE_L_TREASURE);
            pthread_mutex_unlock(&server_data.update_vs_input_mutex);
        }

        // Przesówanie mapy
        else if(c==KEY_UP)
            map_shift(&server_data.map, 0, -MAP_SHIFT_JUMP_Y);
        else if(c==KEY_DOWN)
            map_shift(&server_data.map, 0, MAP_SHIFT_JUMP_Y);
        else if(c==KEY_LEFT)
            map_shift(&server_data.map, -MAP_SHIFT_JUMP_X, 0);
        else if(c==KEY_RIGHT)
            map_shift(&server_data.map, MAP_SHIFT_JUMP_X, 0);
    }
}

// wątek komunikujący się z klientami i aktualizujący dane na serwerze
void *server_update_thread(void *ptr)
{
    pthread_mutex_lock(&server_data.update_vs_input_mutex);

    while(1)
    {
        // W tej pętli odbywa się odczytywanie chęci ruchów wszystkich klientów
        for(int i=0; i<MAX_CLIENTS_COUNT; i++)
        {
            // Blok pamięci sm z danymi danego klienta
            struct client_sm_block_t *client_block = sm_block->clients+i;

            enter_cs(&client_block->data_cs);

            // Wartości w bloku sm
            enum client_type_t type_block = client_block->data_block.client_type;
            int pid_block = client_block->data_block.client_pid;

            // Wartości w danych serwera
            enum client_type_t type_server = server_data.clients_data[i].type;
            int pid_server = server_data.clients_data[i].pid;

            // Slot w bloku sm jest wolny
            if(type_block == CLIENT_TYPE_FREE)
            {   
                // Klient wyszedł z gry
                if(type_server != CLIENT_TYPE_FREE)
                {
                    SERVER_ADD_LOG("Client pid=%d exited", server_data.clients_data[i].pid);
                    sd_remove_client(&server_data, i);
                }
            }

            // Slot na serwerze jest zajety
            else
            {
                // Sprawdzenie flagi obecności - gdyby klient nie został zamknięty naturalnie
                if(!client_block->input_block.respond_flag)
                {
                    SERVER_ADD_LOG("Client pid=%d doesn't respond", server_data.clients_data[i].pid);
                    sd_remove_client(&server_data, i);
                    sm_block->clients[i].data_block.client_type = CLIENT_TYPE_FREE;
                    
                    // Naprawa semaforu, który po nienaturalnym zamknięciu klienta jest sygnalizujący
                    sem_wait(&client_block->output_block_sem);
                }

                // Klient wyszedł z gry(ale inny zdążył zająć jego miejsce)
                if(type_server != CLIENT_TYPE_FREE && pid_block != pid_server)
                {
                    SERVER_ADD_LOG("Client pid=%d exited", server_data.clients_data[i].pid);
                    sd_remove_client(&server_data, i);
                }

                // Klient dołączył do gry
                if(type_server == CLIENT_TYPE_FREE)
                {
                    SERVER_ADD_LOG("Client pid=%d joined", pid_block);
                    sd_add_client(&server_data, i, pid_block, type_block);
                }

                // Odczytanie co chce zrobić klient w tej turze
                enum action_t action = client_block->input_block.action;
                sd_move(&server_data, i, action);
                client_block->input_block.action = ACTION_DO_NOTHING;

                // Resetowanie flagi odpowiedzi
                client_block->input_block.respond_flag = 0;
            }

            exit_cs(&client_block->data_cs);
        }

        struct map_t complete_map;
        sd_create_complete_map(&server_data, &complete_map);

        // Aktualizacja bestii
        sd_update_beasts(&server_data);

        // W tej pętli odbywa się wysyłanie feedbacku do wszystkich klientów
        for(int i=0; i<MAX_CLIENTS_COUNT; i++)
        {
            // Blok pamięci sm z danymi danego klienta
            struct client_sm_block_t *client_block = sm_block->clients+i;

            enter_cs(&client_block->data_cs);

            // Wartości w bloku sm
            enum client_type_t type_block = client_block->data_block.client_type;
            int pid_block = client_block->data_block.client_pid;

            // Wartości w danych serwera
            enum client_type_t type_server = server_data.clients_data[i].type;
            int pid_server = server_data.clients_data[i].pid;

            // Slot na serwerze jest zajety i zajmujący go klient został wcześniej odnotowany w danych serwera
            if(type_block != CLIENT_TYPE_FREE && type_server != CLIENT_TYPE_FREE && pid_block == pid_server)
            {
                // Wysłanie feedbacku
                sd_fill_output_block(&server_data, i, &complete_map, &client_block->output_block);
                sem_post(&client_block->output_block_sem);
            }

            exit_cs(&client_block->data_cs);
        }

        // Nowa runda
        if(sd_is_everything_colected(&server_data))
        {
            SERVER_ADD_LOG("Next round");
            sd_next_round(&server_data);
        }

        // Wyświetlenie okien
        server_display_stats();
        server_display_logs();
        display_help_window(help_window);
        map_display(&complete_map, map_window);

        pthread_mutex_unlock(&server_data.update_vs_input_mutex);

        // Czas trwania jednej tury
        usleep(TURN_TIME);
    }
}

// Inicjowanie ncurses i okien
void server_init_ncurses(void)
{
    setlocale(LC_ALL, "");
	initscr();
  	noecho();
    curs_set(FALSE);
    keypad(stdscr, TRUE);
    cbreak();

    init_colors();

    stat_window = newwin(36, 30, 0, 0);
    log_window = newwin(LOG_LINES_COUNT+1, LOG_LINE_WIDTH, 38, 0);
    map_window = newwin(MAP_VIEW_HEIGHT+2, MAP_VIEW_WIDTH+4, 4, 40);    
    help_window = newwin(18, 21, 4, MAP_VIEW_WIDTH+4+40+8);

    bkgd(COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    refresh();
    wbkgdset(stat_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    wbkgdset(map_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    wbkgdset(log_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    wbkgdset(help_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
}

// Przygotowywanie pamięci współdzielonej
void server_init_sm(void)
{
    fd = shm_open(SHM_FILE_NAME, O_CREAT | O_RDWR, 0600);
    check(fd!=-1, "shm_open error");

    int res = ftruncate(fd, SHARED_BLOCK_SIZE);
    check(res!=-1, "ftruncate error");

    sm_block = (struct clients_sm_block_t *)mmap(NULL, SHARED_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(sm_block!=MAP_FAILED, "mmap error");

    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        struct client_sm_block_t *client_block = sm_block->clients+i;

        client_block->data_block.client_pid = 0;
        client_block->data_block.client_type = CLIENT_TYPE_FREE;
        client_block->input_block.action = ACTION_DO_NOTHING;

        sem_init(&client_block->data_cs, 1, 1);
        sem_init(&client_block->output_block_sem, 1, 0);
    }
}

// Wyświetlenie statystyk serwera
void server_display_stats(void)
{
    werase(stat_window);

    wattron(stat_window, COLOR_PAIR(COLOR_WHITE_ON_RED));
    mvwprintw(stat_window, 0, 0, "Servers PID  : %d", server_data.server_pid);
    wattron(stat_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    mvwprintw(stat_window, 1, 0, "Campside X/Y : %d/%d", server_data.map.campside_x, server_data.map.campside_y);
    mvwprintw(stat_window, 2, 0, "Round Number : %d", server_data.round);

    int line = 5;

    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        enum client_type_t type = server_data.clients_data[i].type;

        wattron(stat_window, COLOR_PAIR(COLOR_WHITE_ON_RED));
        mvwprintw(stat_window, line++, 0, "--PLAYER %d--", i+1);
        wattron(stat_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));

        const char *message = NULL;
        if(type==CLIENT_TYPE_FREE) message="----";
        else if(type==CLIENT_TYPE_HUMAN) message="HUMAN";
        else if(type==CLIENT_TYPE_CPU) message="CPU";
        mvwprintw(stat_window, line++, 0, "Type:   %s", message);

        if(type==CLIENT_TYPE_FREE)
        {
            mvwprintw(stat_window, line++, 0, "PID:    ----");
            mvwprintw(stat_window, line++, 0, "Number: ----");
            mvwprintw(stat_window, line++, 0, "Pos:    ----");
            mvwprintw(stat_window, line++, 0, "Deaths: ----");
            mvwprintw(stat_window, line++, 0, "Coins:  ----");
            line++;
        }

        else
        {
            struct server_client_data_t *client_data = server_data.clients_data+i;

            mvwprintw(stat_window, line++, 0, "PID:    %d", client_data->pid);
            mvwprintw(stat_window, line++, 0, "Number: %d", i+1);
            mvwprintw(stat_window, line++, 0, "Pos:    %d/%d", client_data->current_x, client_data->current_y);
            mvwprintw(stat_window, line++, 0, "Deaths: %d", client_data->deaths);
            mvwprintw(stat_window, line++, 0, "Coins:  %d/%d", client_data->coins_found, client_data->coins_brought);
            line++;
        }
    }
    wrefresh(stat_window);
}

// Wyświetlenie logów serwera
void server_display_logs(void)
{
    werase(log_window);
    int line=0;
    wattron(log_window, COLOR_PAIR(COLOR_WHITE_ON_RED));
    mvwprintw(log_window, line++, 0, "-----------Logs-----------");
    wattron(log_window, COLOR_PAIR(COLOR_BLACK_ON_WHITE));
    for(int i=0; i<LOG_LINES_COUNT; i++)
        mvwprintw(log_window, line++, 0, logs[i]);
    wrefresh(log_window);
}

// Funkcja main
int main(void)
{
    // Inicjacja
    srand(time(NULL));
    sd_init(&server_data);
    server_init_ncurses();
    server_init_sm();
    sd_next_round(&server_data);

    SERVER_ADD_LOG("Starting Server, pid=%d", server_data.server_pid);

    // Tworzenie wątków
    pthread_create(&input_thread, NULL, server_input_thread, NULL);
    pthread_create(&update_thread, NULL, server_update_thread, NULL);

    pthread_join(input_thread, NULL);
    pthread_cancel(update_thread);

    // Sprzątanie
    munmap(sm_block, SHARED_BLOCK_SIZE);
    close(fd);
    shm_unlink(SHM_FILE_NAME);
    endwin();
    delwin(log_window);
    delwin(stat_window);
    delwin(map_window);
    delwin(help_window);
    return 0;
}