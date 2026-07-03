// Test Unicode pass-through in the preprocessor.
// This file contains Unicode characters; please do not "fix" them!

#define COPYRIGHT Copyright © 2012
#define XSTR(X) #X
#define STR(X) XSTR(X)

static const char *copyright = STR(COPYRIGHT);

COPYRIGHT
