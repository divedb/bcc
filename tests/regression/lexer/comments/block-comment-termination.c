// Test that /*/ inside a block comment doesn't close it — only */ does.

int i = /*/ */ 1;

// A normal block comment.
int j = 2; /* comment */ int k = 3;

// Block comment with escaped newline (backslash followed by space, so
// no splice — the \   / sequence doesn't close the comment).
/* no close *\  
still in comment 
*/
int after = 1;
