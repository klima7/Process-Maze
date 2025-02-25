#ifndef __SERVER_DATA_H__
#define __SERVER_DATA_H__

#include <stdint.h>
#include <vector>
#include <pthread.h>
#include "server_data.h"
#include "common.h"
#include "map.h"
#include "beast.h"
#include "tiles.h"

// Dane klienta po stronie serwera
struct server_client_data_t
{
    int pid;
    enum client_type_t type;

    int spawn_x;
    int spawn_y;

    int current_x;
    int current_y;

    int coins_found;
    int coins_brought;

    int deaths;

    int turns_to_wait;
};

// Dane dropu
struct server_drop_data_t
{
    int x;
    int y;
    int value;
};

// Dane monety/skarbu
struct server_something_data_t
{
    int x;
    int y;
};

// Wszystkie dane serwera
struct server_data_t
{
    int server_pid;
    int round;

    struct map_t map;

    // Mamy jedynie dwa wątki update i input
    pthread_mutex_t update_vs_input_mutex;
    
    std::vector<struct server_drop_data_t> dropped_data;
    std::vector<struct server_something_data_t> treasures_s_data;
    std::vector<struct server_something_data_t> treasures_l_data;
    std::vector<struct server_something_data_t> coins_data;
    std::vector<struct beast_t> beasts;

    struct server_client_data_t clients_data[MAX_CLIENTS_COUNT];
};

// Prototypy
void sd_init(struct server_data_t *data);
void sd_add_client(struct server_data_t *data, int slot, int pid, enum client_type_t type);
void sd_remove_client(struct server_data_t *data, int slot);
void sd_move(struct server_data_t *data, int slot, enum action_t action);
void sd_fill_output_block(struct server_data_t *sd, int slot, struct map_t *complete_map, struct client_output_block_t *output);
void sd_set_player_spawn(struct server_data_t *sd, int slot);
void sd_next_round(struct server_data_t *sd);
void sd_create_complete_map(struct server_data_t *sd, struct map_t *result_map);
void sd_player_kill(struct server_data_t *sd, int slot);
void sd_fill_surrounding_area(struct map_t *complete_map, int cx, int cy, surrounding_area_t *area);
void sd_add_something(struct server_data_t *sd, enum tile_t tile);
void sd_add_beast(struct server_data_t *sd);
void sd_move_beast(struct server_data_t *sd, struct beast_t *beast, enum action_t action);
void sd_update_beasts(struct server_data_t *sd);
void sd_generate_entities(struct server_data_t *sd);
void sd_reset_all_players(struct server_data_t *sd);
int sd_is_everything_colected(struct server_data_t *sd);

#endif