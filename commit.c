// commit.c — Commit creation and history traversal
//
// Commit object format (stored as text, one field per line):
//
//   tree <64-char-hex-hash>
//   parent <64-char-hex-hash>        ← omitted for the first commit
//   author <name> <unix-timestamp>
//   committer <name> <unix-timestamp>
//
//   <commit message>
//
// Note: there is a blank line between the headers and the message.
//
// PROVIDED functions: commit_parse, commit_serialize, commit_walk, head_read, head_update
// TODO functions:     commit_create
#include <string.h>
#include "commit.h"
#include "index.h"
#include <sys/stat.h>
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Parse raw commit data into a Commit struct.
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    memset(commit_out, 0, sizeof(*commit_out));

    char *buffer = malloc(len + 1);
    if (!buffer) {
        return -1;
    }

    memcpy(buffer, data, len);
    buffer[len] = '\0';

    char *message_start = strstr(buffer, "\n\n");
    if (message_start) {
        *message_start = '\0';
        message_start += 2;

        snprintf(
            commit_out->message,
            sizeof(commit_out->message),
            "%s",
            message_start
        );
    }

    char *line = strtok(buffer, "\n");

    while (line) {
        if (strncmp(line, "tree ", 5) == 0) {
            if (hex_to_hash(line + 5, &commit_out->tree) != 0) {
                free(buffer);
                return -1;
            }
        } else if (strncmp(line, "parent ", 7) == 0) {
            if (hex_to_hash(line + 7, &commit_out->parent) != 0) {
                free(buffer);
                return -1;
            }
            commit_out->has_parent = 1;
        } else if (strncmp(line, "author ", 7) == 0) {
            snprintf(commit_out->author, sizeof(commit_out->author), "%s", line + 7);
        } else if (strncmp(line, "timestamp ", 10) == 0) {
            commit_out->timestamp = strtoull(line + 10, NULL, 10);
        }

        line = strtok(NULL, "\n");
    }

    free(buffer);
    return 0;
}
// Serialize a Commit struct to the text format.
// Caller must free(*data_out).
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);

    char parent_hex[HASH_HEX_SIZE + 1];
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
    }

    char *buffer = malloc(8192);
    if (!buffer) {
        return -1;
    }

    int len = 0;

    len += snprintf(
        buffer + len,
        8192 - len,
        "tree %s\n",
        tree_hex
    );

    if (commit->has_parent) {
        len += snprintf(
            buffer + len,
            8192 - len,
            "parent %s\n",
            parent_hex
        );
    }

    len += snprintf(
        buffer + len,
        8192 - len,
        "author %s\n"
        "timestamp %llu\n"
        "\n"
        "%s",
        commit->author,
        (unsigned long long)commit->timestamp,
        commit->message
    );

    *data_out = buffer;
    *len_out = len;

    return 0;
}
// Walk commit history from HEAD to the root.
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;

        Commit c;
        int rc = commit_parse(raw, raw_len, &c);
        free(raw);
        if (rc != 0) return -1;

        callback(&id, &c, ctx);

        if (!c.has_parent) break;
        id = c.parent;
    }
    return 0;
}

// Read the current HEAD commit hash.
int head_read(ObjectID *id_out) {
    FILE *f = fopen(".pes/refs/heads/main", "r");
    if (!f) {
        return -1;
    }

    char hex[HASH_HEX_SIZE + 1];

    if (!fgets(hex, sizeof(hex), f)) {
        fclose(f);
        return -1;
    }

    fclose(f);

    char *newline = strchr(hex, '\n');
    if (newline) {
        *newline = '\0';
    }

    return hex_to_hash(hex, id_out);
}
// Update the current branch ref to point to a new commit atomically.
int head_update(const ObjectID *new_commit) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);

    mkdir(".pes/refs", 0755);
    mkdir(".pes/refs/heads", 0755);

    FILE *f = fopen(".pes/refs/heads/main", "w");
    if (!f) {
        return -1;
    }

    fprintf(f, "%s\n", hex);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return 0;
}
// ─── TODO: Implement these ───────────────────────────────────────────────────

// Create a new commit from the current staging area.
//
// HINTS - Useful functions to call:
//   - tree_from_index   : writes the directory tree and gets the root hash
//   - head_read         : gets the parent commit hash (if any)
//   - pes_author        : retrieves the author name string (from pes.h)
//   - time(NULL)        : gets the current unix timestamp
//   - commit_serialize  : converts the filled Commit struct to a text buffer
//   - object_write      : saves the serialized text as OBJ_COMMIT
//   - head_update       : moves the branch pointer to your new commit
//
// Returns 0 on success, -1 on error.
int commit_create(const char *message, ObjectID *commit_id_out) {
    Commit commit;
    memset(&commit, 0, sizeof(commit));

    if (tree_from_index(&commit.tree) != 0) {
        printf("FAIL tree_from_index\n");
        return -1;
    }

    if (head_read(&commit.parent) == 0) {
        commit.has_parent = 1;
    } else {
        commit.has_parent = 0;
    }

    snprintf(commit.author, sizeof(commit.author), "%s", "PES User <pes@localhost>");
    commit.timestamp = (uint64_t)time(NULL);
    snprintf(commit.message, sizeof(commit.message), "%s", message);

    void *data;
    size_t len;

    if (commit_serialize(&commit, &data, &len) != 0) {
        printf("FAIL commit_serialize\n");
        return -1;
    }

    if (object_write(OBJ_COMMIT, data, len, commit_id_out) != 0) {
        printf("FAIL object_write\n");
        free(data);
        return -1;
    }

    free(data);

    if (head_update(commit_id_out) != 0) {
        printf("FAIL head_update\n");
        return -1;
    }

    printf("commit_create success\n");
    return 0;
}