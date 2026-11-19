// Host tool for tools/browser_fonts.py: a font cut down to the code points
// listed in a file, one hexadecimal number per line, with HarfBuzz's
// subsetter (built from the pinned HarfBuzz source the browser links).
//   font-subset FONT CODEPOINTS OUTPUT
#include "hb-subset.h"
#include "hb.h"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: font-subset FONT CODEPOINTS OUTPUT\n");
        return 2;
    }
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(argv[1]);
    if (!blob) {
        std::fprintf(stderr, "font-subset: cannot read %s\n", argv[1]);
        return 1;
    }
    hb_face_t *face = hb_face_create(blob, 0);
    hb_subset_input_t *input = hb_subset_input_create_or_fail();
    FILE *list = std::fopen(argv[2], "r");
    if (!input || !list) {
        std::fprintf(stderr, "font-subset: cannot read %s\n", argv[2]);
        return 1;
    }
    hb_set_t *unicodes = hb_subset_input_unicode_set(input);
    unsigned codepoint;
    while (std::fscanf(list, "%x", &codepoint) == 1) hb_set_add(unicodes, codepoint);
    bool listed = std::feof(list);
    std::fclose(list);
    if (!listed) {
        std::fprintf(stderr, "font-subset: %s is not a list of hexadecimal code points\n", argv[2]);
        return 1;
    }
    hb_face_t *subset = hb_subset_or_fail(face, input);
    if (!subset) {
        std::fprintf(stderr, "font-subset: HarfBuzz could not subset %s\n", argv[1]);
        return 1;
    }
    hb_blob_t *result = hb_face_reference_blob(subset);
    unsigned length = 0;
    const char *data = hb_blob_get_data(result, &length);
    FILE *output = std::fopen(argv[3], "wb");
    if (!output || !length || std::fwrite(data, 1, length, output) != length ||
        std::fclose(output)) {
        std::fprintf(stderr, "font-subset: cannot write %s\n", argv[3]);
        return 1;
    }
    return 0;
}
