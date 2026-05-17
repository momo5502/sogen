#include <stdio.h>

int main(void)
{
    printf("Hello from PIE binary!\n");
    printf("main is at: %p\n", (void*)main);
    return 0;
}
