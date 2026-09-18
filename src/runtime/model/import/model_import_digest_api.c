#include "model_import.h"

bool bongo_cat_import_candidate_inspect(const BongoCatImportCandidate *candidate,
    char output[65], bool *placeholder, BongoCatError *error) {
    return bongo_cat_import_candidate_inspect_cached(candidate, output,
        placeholder, NULL, error);
}

bool bongo_cat_import_candidate_digest(const BongoCatImportCandidate *candidate,
    char output[65], BongoCatError *error) {
    return bongo_cat_import_candidate_inspect(candidate, output, NULL, error);
}
