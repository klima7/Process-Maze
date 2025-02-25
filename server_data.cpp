#include <unistd.h>
#include <stdlib.h>
#include "server_data.h"
#include "common.h"
#include "map.h"
#include "beast.h"
#include "tiles.h"

// Inicjowanie danych serwera
void sd_init(struct server_data_t *data)
{
    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        struct server_client_data_t *client_data = data->clients_data + i;
        client_data->type = CLIENT_TYPE_FREE;
    }

    data->server_pid = getpid();
    data->round = 0;

    pthread_mutex_init(&data->update_vs_input_mutex, NULL);
}

// Dodanie klienta do gry na danych slocie
void sd_add_client(struct server_data_t *data, int slot, int pid, enum client_type_t type)
{
    struct server_client_data_t *client_data = data->clients_data + slot;
    client_data->type = type;
    client_data->pid = pid;
    client_data->turns_to_wait = 0;
    client_data->coins_found = 0;
    client_data->coins_brought = 0;
    client_data->deaths = 0;
    sd_set_player_spawn(data, slot);
}

// Usunięcie klienta z danego slotu z gry
void sd_remove_client(struct server_data_t *data, int slot)
{
    struct server_client_data_t *client_data = data->clients_data + slot;
    client_data->type = CLIENT_TYPE_FREE;
}

// Ruch gracza
void sd_move(struct server_data_t *sd, int slot, enum action_t action)
{
    struct server_client_data_t *client_data = sd->clients_data+slot;

    // Gracz stoi w krzakach
    if(client_data->turns_to_wait>0)
    {
        client_data->turns_to_wait--;
        return;
    }

    // Aktualna pozycja
    int current_x = client_data->current_x;
    int current_y = client_data->current_y;

    // Ewentualna pozycja następna
    int next_x = current_x;
    int next_y = current_y;

    if(action==ACTION_GO_DOWN) next_y++;
    else if(action==ACTION_GO_UP) next_y--;
    else if(action==ACTION_GO_LEFT) next_x--;
    else if(action==ACTION_GO_RIGHT) next_x++;

    // Kafelek docelowy
    enum tile_t dest_tile = map_get_tile(&sd->map, next_x, next_y);

    // Zderzenie ze ścianą - ruch odrzucony
    if(dest_tile==TILE_WALL)
    {
        return;
    }

    // Wpadanie w krzaki - tura czekani
    if(dest_tile==TILE_BUSH && action!=ACTION_DO_NOTHING)
    {
        client_data->turns_to_wait = 1;
    }

    // Zbieranie monet
    for(int i=0; i<(int)sd->coins_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->coins_data.at(i));
        if(sth->x==next_x && sth->y==next_y)
        {
            client_data->coins_found += 1;
            sd->coins_data.erase(sd->coins_data.begin()+i);
        }
    }

    // Zbieranie małych skarbów
    for(int i=0; i<(int)sd->treasures_s_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->treasures_s_data.at(i));
        if(sth->x==next_x && sth->y==next_y)
        {
            client_data->coins_found += SMALL_TREASURE_VALUE;
            sd->treasures_s_data.erase(sd->treasures_s_data.begin()+i);
        }
    }

    // Zbieranie dużych skarbów
    for(int i=0; i<(int)sd->treasures_l_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->treasures_l_data.at(i));
        if(sth->x==next_x && sth->y==next_y)
        {
            client_data->coins_found += BIG_TREASURE_VALUE;
            sd->treasures_l_data.erase(sd->treasures_l_data.begin()+i);
        }
    }

    // Aktualizacja pozycji
    client_data->current_x = next_x;
    client_data->current_y = next_y;

    // Wejście do obozu - W obozie nie obowiązują zderzenia z innymi graczami
    if(client_data->current_x == sd->map.campside_x && client_data->current_y==sd->map.campside_y)
    {
        client_data->coins_brought += client_data->coins_found;
        client_data->coins_found = 0;
    }
    else
    {
        // Zderzenia z innymi graczami
        int kill_player = 0;
        for(int i=0; i<MAX_CLIENTS_COUNT; i++)
        {
            struct server_client_data_t *client_data2 = sd->clients_data+i;
            if(client_data2->type!=CLIENT_TYPE_FREE)
            {
                if(i!=slot && client_data2->current_x==client_data->current_x && client_data2->current_y==client_data->current_y)
                {
                    kill_player = 1;
                    sd_player_kill(sd, i);
                }
            }
        }
        if(kill_player)
            sd_player_kill(sd, slot);
    }

    // Zderzenia z bestiami
    for(int i=0; i<(int)sd->beasts.size(); i++)
    {
        struct beast_t *beast = &(sd->beasts.at(i));
        if(client_data->current_x==beast->x && client_data->current_y==beast->y)
            sd_player_kill(sd, slot);
    }
    
    // Zbieranie dropów
    for(int i=0; i<(int)sd->dropped_data.size(); i++)
    {
        struct server_drop_data_t *drop = &(sd->dropped_data.at(i));
        if(client_data->current_x==drop->x && client_data->current_y==drop->y)
        {
            client_data->coins_found += drop->value;
            sd->dropped_data.erase(sd->dropped_data.begin()+i);
            i--;
        }
    }
}

// Ruch bestii
void sd_move_beast(struct server_data_t *sd, struct beast_t *beast, enum action_t action)
{
    // Bestia stoi w krzakach
    if(beast->turns_to_wait>0)
    {
        beast->turns_to_wait--;
        return;
    }

    // Pozycja aktualna
    int current_x = beast->x;
    int current_y = beast->y;

    // Ewentualna pozycja następna
    int next_x = current_x;
    int next_y = current_y;

    if(action==ACTION_GO_DOWN) next_y++;
    else if(action==ACTION_GO_UP) next_y--;
    else if(action==ACTION_GO_LEFT) next_x--;
    else if(action==ACTION_GO_RIGHT) next_x++;

    // Kafelek docelowy
    enum tile_t dest_tile = map_get_tile(&sd->map, next_x, next_y);

    // Zderzenie ze ścianą
    if(dest_tile==TILE_WALL)
        return;

    // Aktualizacja pozycji
    beast->x = next_x;
    beast->y = next_y;

    // Wpadanie w krzaki
    if(dest_tile==TILE_BUSH && action!=ACTION_DO_NOTHING)
        beast->turns_to_wait = 1;

    // Zderzenie z graczem
    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        struct server_client_data_t *client_data2 = sd->clients_data+i;
        if(client_data2->type!=CLIENT_TYPE_FREE)
        {
            if(client_data2->current_x==beast->x && client_data2->current_y==beast->y)
                sd_player_kill(sd, i);
        }
    }
}

// Wypełnienie bloku danych wysyłanego do klienta
void sd_fill_output_block(struct server_data_t *sd, int slot, struct map_t *complete_map, struct client_output_block_t *output)
{
    struct server_client_data_t *data = sd->clients_data+slot;

    output->x = data->current_x;
    output->y = data->current_y;

    output->coins_brought = data->coins_brought;
    output->coins_found = data->coins_found;

    output->deaths = data->deaths;

    output->round = sd->round;
    output->server_pid = sd->server_pid;

    sd_fill_surrounding_area(complete_map, data->current_x, data->current_y, &output->surrounding_area);
}

// Wylosowanie pozycji spawnu gracza
void sd_set_player_spawn(struct server_data_t *sd, int slot)
{
    struct server_client_data_t *client = sd->clients_data+slot;

    struct map_t complete_map;
    sd_create_complete_map(sd, &complete_map);

    int x = 0;
    int y = 0;

    do
    {
        x = rand()%MAP_WIDTH;
        y = rand()%MAP_HEIGHT;
    }while(map_get_tile(&complete_map, x, y)!=TILE_FLOOR);

    client->spawn_x = x;
    client->spawn_y = y;
    client->current_x = x;
    client->current_y = y;
}

// Wygenerowanie kolejnej rundy
void sd_next_round(struct server_data_t *sd)
{
    sd->round++;

    sd->dropped_data.clear();
    sd->treasures_s_data.clear();
    sd->treasures_l_data.clear();
    sd->coins_data.clear();
    sd->beasts.clear();

    map_generate_everything(&sd->map);
    sd_generate_entities(sd);
    sd_reset_all_players(sd);
}

// Wygenerowanie monet, skarbów, bestii
void sd_generate_entities(struct server_data_t *sd)
{
    int money_count = MAP_HEIGHT*MAP_WIDTH/MAP_GEN_COIN_FACTOR+1;
    for(int i=0; i<money_count; i++)
    {
        int x = 0;
        int y = 0;
        int res = map_random_free_position(&sd->map, &x, &y);
        if(res!=0) break;
        struct server_something_data_t new_something = { x, y };
        sd->coins_data.push_back(new_something);
    }

    int treasure_s_count = MAP_HEIGHT*MAP_WIDTH/MAP_GEN_TREASURE_S_FACTOR+1;
    for(int i=0; i<treasure_s_count; i++)
    {
        int x = 0;
        int y = 0;
        int res = map_random_free_position(&sd->map, &x, &y);
        if(res!=0) break;
        struct server_something_data_t new_something = { x, y };
        sd->treasures_s_data.push_back(new_something);
    }

    int treasure_l_count = MAP_HEIGHT*MAP_WIDTH/MAP_GEN_TREASURE_L_FACTOR+1;
    for(int i=0; i<treasure_l_count; i++)
    {
        int x = 0;
        int y = 0;
        int res = map_random_free_position(&sd->map, &x, &y);
        if(res!=0) break;
        struct server_something_data_t new_something = { x, y };
        sd->treasures_l_data.push_back(new_something);
    }

    int beasts_count = MAP_HEIGHT*MAP_WIDTH/MAP_GEN_BEAST_FACTOR+1;
    for(int i=0; i<beasts_count; i++)
    {
        sd_add_beast(sd);
    }
}

// Zresetowanie wszystkich graczy
void sd_reset_all_players(struct server_data_t *sd)
{
    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        struct server_client_data_t *client = sd->clients_data+i;
        if(client->type!=CLIENT_TYPE_FREE)
        {
            sd_set_player_spawn(sd, i);
            client->coins_brought = 0;
            client->coins_found = 0;
            client->deaths = 0;
            client->turns_to_wait = 0;

        }
    }
}

// Utworzenie pełnej mapy - uwzględniającej listy monet, skarbów, bestii...
void sd_create_complete_map(struct server_data_t *sd, struct map_t *result_map)
{
    result_map->viewpoint_x = sd->map.viewpoint_x;
    result_map->viewpoint_y = sd->map.viewpoint_y;

    // Odbijanie tła mapy
    map_copy(&sd->map, result_map);

    // Odbijanie graczy na mapie
    for(int i=0; i<MAX_CLIENTS_COUNT; i++)
    {
        struct server_client_data_t *client = sd->clients_data+i;
        if(client->type!=CLIENT_TYPE_FREE)
            result_map->map[client->current_y][client->current_x] = (enum tile_t)(TILE_PLAYER1+i);
    }

    // Odbijanie dropu
    for(int i=0; i<(int)sd->dropped_data.size(); i++)
    {
        struct server_drop_data_t *drop = &(sd->dropped_data.at(i));
        result_map->map[drop->y][drop->x] = TILE_DROP;
    }

    // Odbijanie monet
    for(int i=0; i<(int)sd->coins_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->coins_data.at(i));
        result_map->map[sth->y][sth->x] = TILE_COIN;
    }

    // Odbijanie małych skarbów
    for(int i=0; i<(int)sd->treasures_s_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->treasures_s_data.at(i));
        result_map->map[sth->y][sth->x] = TILE_S_TREASURE;
    }

    // Odbijanie dużych skarbów
    for(int i=0; i<(int)sd->treasures_l_data.size(); i++)
    {
        struct server_something_data_t *sth = &(sd->treasures_l_data.at(i));
        result_map->map[sth->y][sth->x] = TILE_L_TREASURE;
    }

    // Odbijanie bestii
    for(int i=0; i<(int)sd->beasts.size(); i++)
    {
        struct beast_t *beast = &(sd->beasts.at(i));
        result_map->map[beast->y][beast->x] = TILE_BEAST;
    }

    // Odbicie obozowiska
    result_map->map[sd->map.campside_y][sd->map.campside_x] = TILE_CAMPSIDE;
}

// Zabicie gracza - upuszcza on drop
void sd_player_kill(struct server_data_t *sd, int slot)
{
    struct server_client_data_t *client = sd->clients_data+slot;

    if(client->coins_found>0)
    {
        int drop_found = 0;
        for(int i=0; i<(int)sd->dropped_data.size(); i++)
        {
            struct server_drop_data_t *drop = &(sd->dropped_data.at(i));
            if(drop->x==client->current_x && drop->y==client->current_y)
            {
                drop->value += client->coins_found;
                drop_found = 1;
                break;
            }
        }

        if(!drop_found)
        {
            struct server_drop_data_t new_drop = { client->current_x, client->current_y, client->coins_found };
            sd->dropped_data.push_back(new_drop);
        }
    }

    client->deaths++;
    client->current_x = client->spawn_x;
    client->current_y = client->spawn_y;
    client->coins_found = 0;
}

// Wypełnienie bloku najbliższego sąsiedztwa gracza - wysyłanego klientowi
void sd_fill_surrounding_area(struct map_t *complete_map, int cx, int cy, surrounding_area_t *area)
{
    for(int i=0; i<VISIBLE_AREA_SIZE; i++)
    {
        for(int j=0; j<VISIBLE_AREA_SIZE; j++)
        {
            int rel_y = i-VISIBLE_DISTANCE;
            int rel_x = j-VISIBLE_DISTANCE;

            enum tile_t tile = map_get_tile(complete_map, cx+rel_x, cy+rel_y);
            (*area)[i][j] = tile;
        }
    }
}

// Dodanie monety/skarbu
void sd_add_something(struct server_data_t *sd, enum tile_t tile)
{
    struct map_t complete_map;
    sd_create_complete_map(sd, &complete_map);

    int x = 0;
    int y = 0;

    int res = map_random_free_position(&complete_map, &x, &y);      
    if(res==1) return;

    struct server_something_data_t new_something = { x, y };

    if(tile==TILE_COIN)
        sd->coins_data.push_back(new_something);
    else if(tile==TILE_S_TREASURE)
        sd->treasures_s_data.push_back(new_something);
    else if(tile==TILE_L_TREASURE)
        sd->treasures_l_data.push_back(new_something);
}

// Dodanie bestii
void sd_add_beast(struct server_data_t *sd)
{
    struct map_t complete_map;
    sd_create_complete_map(sd, &complete_map);

    int x = 0;
    int y = 0;

    do
    {
        x = rand()%MAP_WIDTH;
        y = rand()%MAP_HEIGHT;
    }while(map_get_tile(&complete_map, x, y)!=TILE_FLOOR);

    struct beast_t beast;
    beast_init(&beast, x, y);
    sd->beasts.push_back(beast);
}

// Aktualizacja wszystkich bestii
void sd_update_beasts(struct server_data_t *sd)
{
    for(int i=0; i<(int)sd->beasts.size(); i++)
        beast_update(sd, i);
}

// Sprawdzenie czy monety i skarby zostały pozbierane i nowa runda może być generowana
int sd_is_everything_colected(struct server_data_t *sd)
{
    if(sd->treasures_s_data.empty() && sd->treasures_l_data.empty() && sd->coins_data.empty() && sd->dropped_data.empty()) 
        return 1;
    return 0;
}
