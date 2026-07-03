// Test escaped newlines in BCPL (//) comments.
//
// Backslash immediately before newline splices the lines, so the
// comment continues past the physical newline.
//\
int should_be_commented = 1;

// A normal // comment (no backslash at end) ends at the newline.
int should_be_active = 2;

int result = should_be_active;
