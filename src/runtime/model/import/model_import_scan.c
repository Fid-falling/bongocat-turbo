#include "model_import.h"
#include "model_import_probe.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMPORT_CHILD_CAP 64
#define IMPORT_ENTRY_CAP 256
#define IMPORT_NODE_CAP 256
#define IMPORT_SCAN_DEPTH 3
#define IMPORT_SCAN_BUDGET_NS 2000000000ull

typedef struct ChildList {
    char paths[IMPORT_CHILD_CAP][BONGO_CAT_PATH_CAP];
    size_t count, entries;
    bool limited;
} ChildList;

typedef struct ImportWorkspace {
    BongoCatImportDiscovery discovery;
    ChildList children;
} ImportWorkspace;

typedef struct ImportScan {
    BongoCatImportVisitor visitor;
    void *userdata;
    BongoCatError *error;
    size_t nodes;
    uint64_t deadline;
    bool limited;
    bool diagnostic;
} ImportScan;

typedef struct ImportQueue {
    char paths[IMPORT_NODE_CAP][BONGO_CAT_PATH_CAP];
    unsigned char depths[IMPORT_NODE_CAP];
    size_t head, count;
} ImportQueue;

static BongoCatPathVisit collect_child(void *userdata,
    const char *dirname, const char *name) {
    ChildList *list = userdata;
    if (++list->entries > IMPORT_ENTRY_CAP || list->count >= IMPORT_CHILD_CAP) {
        list->limited = true; return BONGO_CAT_PATH_SUCCESS;
    }
    if (name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), dirname, name) &&
        bongo_cat_path_is_dir(path))
        snprintf(list->paths[list->count++], BONGO_CAT_PATH_CAP, "%s", path);
    return BONGO_CAT_PATH_CONTINUE;
}

static int compare_path(const void *left, const void *right) {
#ifdef _WIN32
    return SDL_strcasecmp(left, right);
#else
    return strcmp(left, right);
#endif
}

static bool within_budget(ImportScan *scan) {
    if (scan->nodes >= IMPORT_NODE_CAP || SDL_GetTicksNS() >= scan->deadline) {
        scan->limited = true; return false;
    }
    scan->nodes++; return true;
}

static void enqueue_children(ImportScan *scan, ImportQueue *queue,
    const ChildList *children, int depth) {
    for (size_t i = 0; i < children->count; ++i) {
        if (queue->count >= IMPORT_NODE_CAP) {
            scan->limited = true; return;
        }
        snprintf(queue->paths[queue->count], BONGO_CAT_PATH_CAP, "%s",
            children->paths[i]);
        queue->depths[queue->count++] = (unsigned char)depth;
    }
}

static BongoCatResult scan_node(ImportScan *scan, ImportQueue *queue,
    const char *source, int depth) {
    if (!within_budget(scan)) return BONGO_CAT_OK;
    uint64_t started = SDL_GetTicksNS();
    if (scan->diagnostic) SDL_Log(
        "[runtime] Model import scan node started: node=%llu depth=%d "
        "path=%s", (unsigned long long)scan->nodes, depth, source);
    ImportWorkspace *work = calloc(1, sizeof(*work));
    if (!work) {
        bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import scan workspace");
        return BONGO_CAT_ERROR_MEMORY;
    }
    BongoCatError local = {0};
    BongoCatImportFormat format = BONGO_CAT_IMPORT_MVER;
    int found = bongo_cat_import_probe_exact(source, &work->discovery,
        &format, BONGO_CAT_IMPORT_PROBE_FALLBACK, scan->diagnostic, &local);
    if (found > 0) {
        if (scan->diagnostic) SDL_Log(
            "[runtime] Model import scan node discovered: format=%s "
            "candidates=%llu elapsed_ms=%.1f path=%s",
            bongo_cat_import_format_name(format),
            (unsigned long long)work->discovery.count,
            (SDL_GetTicksNS() - started) / 1000000.0, source);
        uint64_t visitor_started = SDL_GetTicksNS();
        BongoCatResult result = scan->visitor(scan->userdata, source,
            &work->discovery, scan->error);
        uint64_t finished = SDL_GetTicksNS();
        uint64_t elapsed = finished - visitor_started;
        if (scan->deadline <= UINT64_MAX - elapsed) scan->deadline += elapsed;
        else scan->deadline = UINT64_MAX;
        if (scan->diagnostic) SDL_Log(
            "[runtime] Model import scan node collected: result=%d "
            "elapsed_ms=%.1f path=%s", (int)result,
            (finished - started) / 1000000.0, source);
        free(work); return result;
    }
    if (depth < IMPORT_SCAN_DEPTH) {
        bool enumerated = bongo_cat_path_enumerate(source, collect_child,
            &work->children);
        if (!enumerated) {
            if (!scan->error || scan->error->code == BONGO_CAT_OK)
                bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_IO,
                    "Cannot enumerate model directory: %s", source);
            BongoCatResult result = scan->error && scan->error->code
                ? scan->error->code : BONGO_CAT_ERROR_IO;
            free(work);
            return result;
        }
        if (work->children.limited) scan->limited = true;
        qsort(work->children.paths, work->children.count,
            sizeof(work->children.paths[0]), compare_path);
        enqueue_children(scan, queue, &work->children, depth + 1);
    }
    if (scan->diagnostic) SDL_Log(
        "[runtime] Model import scan node completed: children=%llu "
        "elapsed_ms=%.1f path=%s",
        (unsigned long long)work->children.count,
        (SDL_GetTicksNS() - started) / 1000000.0, source);
    free(work); return BONGO_CAT_OK;
}

static BongoCatResult scan_with_budget(const char *root,
    BongoCatImportVisitor visitor, void *userdata, uint64_t budget_ns,
    bool diagnostic, BongoCatError *error) {
    if (!root || !visitor) return BONGO_CAT_ERROR_ARGUMENT;
    ImportWorkspace *work = calloc(1, sizeof(*work));
    ImportQueue *queue = calloc(1, sizeof(*queue));
    if (!work || !queue) {
        free(work);
        free(queue);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import directory scan");
        return BONGO_CAT_ERROR_MEMORY;
    }
    uint64_t now = SDL_GetTicksNS();
    uint64_t deadline = budget_ns <= UINT64_MAX - now
        ? now + budget_ns : UINT64_MAX;
    ImportScan scan = {
        visitor, userdata, error, 0, deadline, false, diagnostic
    };
    BongoCatResult result = BONGO_CAT_OK;
    if (diagnostic) SDL_Log(
        "[runtime] Model import scan started: budget_ms=%.1f path=%s",
        budget_ns / 1000000.0, root);
    if (bongo_cat_path_enumerate(root, collect_child, &work->children)) {
        if (work->children.limited) scan.limited = true;
        qsort(work->children.paths, work->children.count,
            sizeof(work->children.paths[0]), compare_path);
        enqueue_children(&scan, queue, &work->children, 1);
        while (queue->head < queue->count) {
            size_t current = queue->head++;
            BongoCatResult item = scan_node(&scan, queue,
                queue->paths[current], queue->depths[current]);
            if (item != BONGO_CAT_OK && result == BONGO_CAT_OK) result = item;
        }
    } else {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot enumerate model directory: %s", root);
        result = error && error->code ? error->code : BONGO_CAT_ERROR_IO;
    }
    if (scan.limited) SDL_Log(
        "Model folder scan reached its directory budget");
    if (diagnostic) SDL_Log(
        "[runtime] Model import scan completed: result=%d nodes=%llu "
        "queued=%llu limited=%d elapsed_ms=%.1f path=%s", (int)result,
        (unsigned long long)scan.nodes, (unsigned long long)queue->count,
        scan.limited, (SDL_GetTicksNS() - now) / 1000000.0, root);
    free(queue); free(work); return result;
}

BongoCatResult bongo_cat_import_scan_budget(const char *root,
    BongoCatImportVisitor visitor, void *userdata, uint64_t budget_ns,
    BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata, budget_ns, false, error);
}

BongoCatResult bongo_cat_import_scan(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata,
        IMPORT_SCAN_BUDGET_NS, false, error);
}

BongoCatResult bongo_cat_import_scan_diagnostic(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata,
        IMPORT_SCAN_BUDGET_NS, true, error);
}
