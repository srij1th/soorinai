#include "server.h"

#include "ai.h"
#include "board.h"
#include "user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef int socklen_t;
#define CLOSESOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define CLOSESOCKET close
#endif

#define REQ_BUF 16384
#define RES_BUF 32768
#define MAX_MOVE_LOG 512

typedef struct {
    int logged_in;
    UserProfile user;
    ChessBoard game;
    int game_active;
    char status[64];
    time_t game_start_time;
    int game_duration_seconds;
    int timer_started; /* 1 after first move (player or AI) */
    int player_side;   /* 0 = white, 1 = black */
    int white_score;
    int black_score;
    int last_gain;
    int last_mover; /* 0 = player moved last, 1 = AI, -1 = none */
    Move last_move;
    int has_last_move;
    char move_log[MAX_MOVE_LOG][16];
    int move_log_count;
} AppState;

static AppState g_app;

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int path_is(const char *path, const char *endpoint) {
    size_t n = strlen(endpoint);
    if (strncmp(path, endpoint, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '?';
}

/* Normalize Request-URI: absolute URL, query, fragment, trailing CR/LF */
static void sanitize_request_path(char *path) {
    char *cut;
    size_t len;

    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        const char *after_scheme = strstr(path, "://");
        if (after_scheme) {
            cut = strchr(after_scheme + 3, '/');
            if (cut) {
                memmove(path, cut, strlen(cut) + 1);
            } else {
                path[0] = '/';
                path[1] = '\0';
                return;
            }
        }
    }
    cut = strpbrk(path, "?#");
    if (cut) *cut = '\0';
    len = strlen(path);
    while (len > 0 && (path[len - 1] == '\r' || path[len - 1] == '\n' || path[len - 1] == ' ' ||
                      path[len - 1] == '\t')) {
        path[--len] = '\0';
    }
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static int read_file_text(const char *path, char *out, int out_size) {
    FILE *fp = fopen(path, "rb");
    int n;
    if (!fp) return 0;
    n = (int)fread(out, 1, (size_t)(out_size - 1), fp);
    out[n] = '\0';
    fclose(fp);
    return 1;
}

static int get_json_string(const char *json, const char *key, char *out, int out_size) {
    char pattern[64];
    const char *p;
    const char *start;
    const char *end;
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    start = p + strlen(pattern);
    end = strchr(start, '"');
    if (!end) return 0;
    if ((int)(end - start) >= out_size) return 0;
    strncpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return 1;
}

static int get_json_int(const char *json, const char *key, int *value) {
    char pattern[64];
    const char *p;
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    *value = atoi(p);
    return 1;
}

static int parse_content_length(const char *req) {
    const char *p = strstr(req, "Content-Length:");
    if (!p) return 0;
    p += (int)strlen("Content-Length:");
    while (*p == ' ') p++;
    return atoi(p);
}

static void send_response(SOCKET client, int code, const char *content_type, const char *body) {
    char header[512];
    int len = (int)strlen(body);
    const char *status = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Bad Request";
    snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Connection: close\r\n\r\n",
        code,
        status,
        content_type,
        len
    );
    send(client, header, (int)strlen(header), 0);
    send(client, body, len, 0);
}

static void send_json(SOCKET client, const char *json) {
    send_response(client, 200, "application/json", json);
}

static void send_error_json(SOCKET client, const char *message) {
    char body[512];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    send_response(client, 400, "application/json", body);
}

static void reset_game_tracking(void) {
    g_app.has_last_move = 0;
    g_app.move_log_count = 0;
    g_app.last_move.from_row = 0;
    g_app.last_move.from_col = 0;
    g_app.last_move.to_row = 0;
    g_app.last_move.to_col = 0;
    g_app.last_move.promotion = EMPTY;
    g_app.last_gain = 0;
    g_app.last_mover = -1;
    g_app.white_score = 0;
    g_app.black_score = 0;
    g_app.game_duration_seconds = 240; /* 4 minutes */
    g_app.game_start_time = 0;
    g_app.timer_started = 0;
}

static void maybe_start_timer_after_move(void) {
    if (g_app.timer_started) return;
    g_app.timer_started = 1;
    g_app.game_start_time = time(NULL);
}

static void append_move_log(const Move *m) {
    char text[16];
    if (g_app.move_log_count >= MAX_MOVE_LOG) return;
    move_to_text(m, text, sizeof(text));
    strncpy(g_app.move_log[g_app.move_log_count], text, sizeof(g_app.move_log[0]) - 1);
    g_app.move_log[g_app.move_log_count][sizeof(g_app.move_log[0]) - 1] = '\0';
    g_app.move_log_count++;
    g_app.last_move = *m;
    g_app.has_last_move = 1;
}

static int capture_value_for_move(const ChessBoard *state, const Move *m) {
    Piece moving = state->board[m->from_row][m->from_col];
    Piece target = state->board[m->to_row][m->to_col];
    if ((moving == WP || moving == BP) &&
        m->from_col != m->to_col &&
        target == EMPTY) {
        /* En passant capture. */
        return 1;
    }
    return piece_value(target);
}

static int get_time_left_seconds(void) {
    int elapsed;
    int left;
    if (!g_app.timer_started || g_app.game_start_time == 0) return g_app.game_duration_seconds;
    elapsed = (int)(time(NULL) - g_app.game_start_time);
    left = g_app.game_duration_seconds - elapsed;
    if (left < 0) left = 0;
    return left;
}

static void check_timeout_and_update_result(void) {
    int player_mat;
    int ai_mat;
    if (!g_app.game_active) return;
    if (!g_app.timer_started) return;
    if (get_time_left_seconds() > 0) return;

    player_mat = (g_app.player_side == 0) ? g_app.white_score : g_app.black_score;
    ai_mat = (g_app.player_side == 0) ? g_app.black_score : g_app.white_score;

    g_app.game_active = 0;
    if (player_mat > ai_mat) {
        strcpy(g_app.status, "timeout_player_wins");
        g_app.user.games_played++;
        g_app.user.wins++;
    } else if (ai_mat > player_mat) {
        strcpy(g_app.status, "timeout_ai_wins");
        g_app.user.games_played++;
        g_app.user.losses++;
    } else {
        strcpy(g_app.status, "timeout_draw");
        g_app.user.games_played++;
        g_app.user.draws++;
    }
    save_user_profile(&g_app.user);
}

static void build_board_json(char *out, int out_size) {
    int r, c;
    int n = 0;
    Move legal[MAX_MOVES];
    int count = 0;
    char moves_json[8192];
    char history_json[8192];
    int m;
    int k = 0;
    int h = 0;
    int time_left = get_time_left_seconds();
    int player_mat = (g_app.player_side == 0) ? g_app.white_score : g_app.black_score;
    int ai_mat = (g_app.player_side == 0) ? g_app.black_score : g_app.white_score;
    int ai_side = 1 - g_app.player_side;

    generate_legal_moves(&g_app.game, legal, &count);
    k += snprintf(moves_json + k, sizeof(moves_json) - (size_t)k, "[");
    for (m = 0; m < count; m++) {
        k += snprintf(
            moves_json + k,
            sizeof(moves_json) - (size_t)k,
            "%s{\"fr\":%d,\"fc\":%d,\"tr\":%d,\"tc\":%d}",
            (m == 0) ? "" : ",",
            legal[m].from_row,
            legal[m].from_col,
            legal[m].to_row,
            legal[m].to_col
        );
    }
    k += snprintf(moves_json + k, sizeof(moves_json) - (size_t)k, "]");
    h += snprintf(history_json + h, sizeof(history_json) - (size_t)h, "[");
    for (m = 0; m < g_app.move_log_count; m++) {
        h += snprintf(
            history_json + h,
            sizeof(history_json) - (size_t)h,
            "%s\"%s\"",
            (m == 0) ? "" : ",",
            g_app.move_log[m]
        );
    }
    h += snprintf(history_json + h, sizeof(history_json) - (size_t)h, "]");

    n += snprintf(out + n, (size_t)(out_size - n), "{\"ok\":true,\"board\":[");
    for (r = 0; r < 8; r++) {
        n += snprintf(out + n, (size_t)(out_size - n), "%s[", (r == 0) ? "" : ",");
        for (c = 0; c < 8; c++) {
            n += snprintf(
                out + n,
                (size_t)(out_size - n),
                "%s\"%c\"",
                (c == 0) ? "" : ",",
                piece_to_char(g_app.game.board[r][c])
            );
        }
        n += snprintf(out + n, (size_t)(out_size - n), "]");
    }

    snprintf(
        out + n,
        (size_t)(out_size - n),
        "],\"turn\":\"%s\",\"status\":\"%s\",\"legalMoves\":%s,"
        "\"aiPending\":%s,"
        "\"timeLeft\":%d,"
        "\"timerStarted\":%s,"
        "\"yourTurn\":%s,"
        "\"playerColor\":\"%s\","
        "\"scores\":{\"player\":%d,\"ai\":%d},"
        "\"lastGain\":{\"value\":%d,\"mover\":%d},"
        "\"lastMove\":{\"valid\":%d,\"fr\":%d,\"fc\":%d,\"tr\":%d,\"tc\":%d},"
        "\"moveHistory\":%s}",
        g_app.game.side_to_move == 0 ? "white" : "black",
        g_app.status,
        moves_json,
        (g_app.game_active && g_app.game.side_to_move == ai_side) ? "true" : "false",
        time_left,
        g_app.timer_started ? "true" : "false",
        (g_app.game_active && g_app.game.side_to_move == g_app.player_side) ? "true" : "false",
        g_app.player_side == 0 ? "white" : "black",
        player_mat,
        ai_mat,
        g_app.last_gain,
        g_app.last_mover,
        g_app.has_last_move,
        g_app.last_move.from_row,
        g_app.last_move.from_col,
        g_app.last_move.to_row,
        g_app.last_move.to_col,
        history_json
    );
}

static void update_game_status(void) {
    int current = g_app.game.side_to_move;
    if (is_checkmate(&g_app.game, current)) {
        g_app.game_active = 0;
        g_app.user.games_played++;
        if (current == g_app.player_side) {
            strcpy(g_app.status, "checkmate_player_loses");
            g_app.user.losses++;
        } else {
            strcpy(g_app.status, "checkmate_player_wins");
            g_app.user.wins++;
        }
        save_user_profile(&g_app.user);
        return;
    }
    if (is_stalemate(&g_app.game, current)) {
        strcpy(g_app.status, "stalemate_draw");
        g_app.game_active = 0;
        g_app.user.games_played++;
        g_app.user.draws++;
        save_user_profile(&g_app.user);
        return;
    }
    if (is_in_check(&g_app.game, current)) strcpy(g_app.status, "check");
    else strcpy(g_app.status, "normal");
}

static void handle_api(SOCKET client, const char *path, const char *body) {
    if (path_is(path, "/login")) {
        char username[32], password[32], res[512];
        if (!get_json_string(body, "username", username, sizeof(username)) ||
            !get_json_string(body, "password", password, sizeof(password))) {
            send_error_json(client, "Invalid login JSON.");
            return;
        }
        if (!login_user(username, password, &g_app.user)) {
            send_error_json(client, "Invalid username or password.");
            return;
        }
        g_app.logged_in = 1;
        g_app.game_active = 0;
        strcpy(g_app.status, "normal");
        snprintf(res, sizeof(res), "{\"ok\":true,\"username\":\"%s\"}", g_app.user.username);
        send_json(client, res);
        return;
    }

    if (path_is(path, "/signup")) {
        char username[32], password[32];
        if (!get_json_string(body, "username", username, sizeof(username)) ||
            !get_json_string(body, "password", password, sizeof(password))) {
            send_error_json(client, "Invalid signup JSON.");
            return;
        }
        if (!create_profile(username, password)) {
            send_error_json(client, "User exists or input invalid.");
            return;
        }
        send_json(client, "{\"ok\":true,\"message\":\"Account created.\"}");
        return;
    }

    if (path_is(path, "/logout")) {
        g_app.logged_in = 0;
        g_app.game_active = 0;
        send_json(client, "{\"ok\":true}");
        return;
    }

    if (path_is(path, "/update-stats")) {
        if (g_app.game_active) {
            check_timeout_and_update_result();
            update_game_status();
        }
        send_json(client, "{\"ok\":true}");
        return;
    }

    if (!g_app.logged_in) {
        send_error_json(client, "Please login first.");
        return;
    }

    if (path_is(path, "/start-game")) {
        char color_str[16];
        g_app.player_side = 0;
        if (get_json_string(body, "playerColor", color_str, sizeof(color_str))) {
            if (strcmp(color_str, "black") == 0) g_app.player_side = 1;
            else g_app.player_side = 0;
        }
        init_board(&g_app.game);
        g_app.game_active = 1;
        strcpy(g_app.status, "normal");
        reset_game_tracking();
        send_json(client, "{\"ok\":true}");
        return;
    }

    if (path_is(path, "/move")) {
        int fr, fc, tr, tc;
        Move m;
        char res[RES_BUF];
        int gain;

        if (!g_app.game_active) {
            send_error_json(client, "No active game. Start a game first.");
            return;
        }
        check_timeout_and_update_result();
        if (!g_app.game_active) {
            build_board_json(res, sizeof(res));
            send_json(client, res);
            return;
        }
        if (!get_json_int(body, "fr", &fr) || !get_json_int(body, "fc", &fc) ||
            !get_json_int(body, "tr", &tr) || !get_json_int(body, "tc", &tc)) {
            send_error_json(client, "Invalid move JSON.");
            return;
        }
        m.from_row = fr; m.from_col = fc; m.to_row = tr; m.to_col = tc; m.promotion = EMPTY;
        if (g_app.game.side_to_move != g_app.player_side) {
            send_error_json(client, "Please wait for AI move.");
            return;
        }
        if (!is_move_legal(&g_app.game, &m)) {
            send_error_json(client, "Illegal move.");
            return;
        }
        gain = capture_value_for_move(&g_app.game, &m);
        apply_move(&g_app.game, &m);
        append_move_log(&m);
        maybe_start_timer_after_move();
        if (g_app.player_side == 0) g_app.white_score += gain;
        else g_app.black_score += gain;
        g_app.last_gain = gain;
        g_app.last_mover = 0;
        update_game_status();
        check_timeout_and_update_result();

        build_board_json(res, sizeof(res));
        send_json(client, res);
        return;
    }

    if (path_is(path, "/ai-move")) {
        Move ai_m;
        char res[RES_BUF];
        int gain;
        if (!g_app.game_active) {
            send_error_json(client, "No active game.");
            return;
        }
        check_timeout_and_update_result();
        if (!g_app.game_active) {
            build_board_json(res, sizeof(res));
            send_json(client, res);
            return;
        }
        if (g_app.game.side_to_move != (1 - g_app.player_side)) {
            build_board_json(res, sizeof(res));
            send_json(client, res);
            return;
        }
        ai_m = choose_ai_move(&g_app.game);
        if (!is_move_legal(&g_app.game, &ai_m)) {
            send_error_json(client, "AI could not find legal move.");
            return;
        }
        gain = capture_value_for_move(&g_app.game, &ai_m);
        apply_move(&g_app.game, &ai_m);
        append_move_log(&ai_m);
        maybe_start_timer_after_move();
        if (g_app.player_side == 0) g_app.black_score += gain;
        else g_app.white_score += gain;
        g_app.last_gain = gain;
        g_app.last_mover = 1;
        update_game_status();
        check_timeout_and_update_result();
        build_board_json(res, sizeof(res));
        send_json(client, res);
        return;
    }

    if (path_is(path, "/get-board")) {
        char res[RES_BUF];
        if (!g_app.game_active) {
            send_error_json(client, "No active game.");
            return;
        }
        check_timeout_and_update_result();
        build_board_json(res, sizeof(res));
        send_json(client, res);
        return;
    }

    if (path_is(path, "/get-stats")) {
        char res[512];
        UserProfile disk_user;
        if (load_user_profile_by_username(g_app.user.username, &disk_user)) {
            g_app.user.games_played = disk_user.games_played;
            g_app.user.wins = disk_user.wins;
            g_app.user.losses = disk_user.losses;
            g_app.user.draws = disk_user.draws;
        }
        snprintf(
            res,
            sizeof(res),
            "{\"ok\":true,\"username\":\"%s\",\"gamesPlayed\":%d,\"wins\":%d,\"losses\":%d,\"draws\":%d}",
            g_app.user.username,
            g_app.user.games_played,
            g_app.user.wins,
            g_app.user.losses,
            g_app.user.draws
        );
        send_json(client, res);
        return;
    }

    if (path_is(path, "/settings")) {
        int key_style = g_app.user.keybinding_style;
        int board_style = g_app.user.board_style;
        char res[256];

        get_json_int(body, "keybinding_style", &key_style);
        get_json_int(body, "board_style", &board_style);
        g_app.user.keybinding_style = key_style;
        g_app.user.board_style = board_style;
        save_user_profile(&g_app.user);
        snprintf(res, sizeof(res), "{\"ok\":true,\"keybinding_style\":%d,\"board_style\":%d}", key_style, board_style);
        send_json(client, res);
        return;
    }

    if (path_is(path, "/reset-stats")) {
        if (!reset_user_stats(&g_app.user)) {
            send_error_json(client, "Could not reset stats.");
            return;
        }
        send_json(client, "{\"ok\":true}");
        return;
    }

    fprintf(stderr, "Unknown API path (check URL): %s\n", path);
    send_error_json(client, "Unknown API endpoint.");
}

static void serve_file(SOCKET client, const char *path) {
    char body[RES_BUF];
    const char *ctype = "text/plain";
    const char *file_path = path;

    if (strcmp(path, "/") == 0) file_path = "/index.html";
    if (file_path[0] == '/') file_path++;

    if (!file_exists(file_path)) {
        send_response(client, 404, "text/plain", "404 - File not found");
        return;
    }

    if (!read_file_text(file_path, body, sizeof(body))) {
        send_response(client, 400, "text/plain", "Could not read file");
        return;
    }

    if (strstr(file_path, ".html")) ctype = "text/html";
    else if (strstr(file_path, ".css")) ctype = "text/css";
    else if (strstr(file_path, ".js")) ctype = "application/javascript";

    send_response(client, 200, ctype, body);
}

static void handle_client(SOCKET client) {
    char req[REQ_BUF];
    char method[8] = {0};
    char path[256] = {0};
    char *body;
    int n = 0;
    int got;
    int content_len = 0;
    int header_len = -1;

    while (n < (int)sizeof(req) - 1) {
        got = recv(client, req + n, (int)sizeof(req) - 1 - n, 0);
        if (got <= 0) break;
        n += got;
        req[n] = '\0';

        if (header_len < 0) {
            char *header_end = strstr(req, "\r\n\r\n");
            if (header_end) {
                header_len = (int)(header_end - req) + 4;
                content_len = parse_content_length(req);
                if (n >= header_len + content_len) break;
            }
        } else if (n >= header_len + content_len) {
            break;
        }
    }
    if (n <= 0) return;
    req[n] = '\0';

    sscanf(req, "%7s %255s", method, path);
    sanitize_request_path(path);
    body = strstr(req, "\r\n\r\n");
    if (body) body += 4;
    else body = "";

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client, 200, "text/plain", "");
        return;
    }

    if (path_is(path, "/login") || path_is(path, "/signup") || path_is(path, "/start-game") ||
        path_is(path, "/move") || path_is(path, "/ai-move") || path_is(path, "/get-board") ||
        path_is(path, "/get-stats") || path_is(path, "/settings") || path_is(path, "/reset-stats") ||
        path_is(path, "/update-stats") || path_is(path, "/logout")) {
        handle_api(client, path, body);
    } else {
        serve_file(client, path);
    }
}

int run_http_server(int port) {
    SOCKET server_fd, client_fd;
    struct sockaddr_in addr;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }
#endif

    memset(&g_app, 0, sizeof(g_app));
    strcpy(g_app.status, "normal");

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        printf("Socket creation failed.\n");
        return 1;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("Bind failed. Port %d may be in use.\n", port);
        CLOSESOCKET(server_fd);
        return 1;
    }
    if (listen(server_fd, 10) == SOCKET_ERROR) {
        printf("Listen failed.\n");
        CLOSESOCKET(server_fd);
        return 1;
    }

    printf("Chess server running at http://localhost:%d\n", port);
    printf("Open browser: http://localhost:%d/index.html\n", port);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == INVALID_SOCKET) continue;
#ifdef _WIN32
        {
            DWORD timeout_ms = 3000;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
        }
#else
        {
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
#endif
        handle_client(client_fd);
        CLOSESOCKET(client_fd);
    }

    CLOSESOCKET(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
