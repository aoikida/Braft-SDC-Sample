#include <stdio.h>

int main() {
    int flag = 1;
    int result;

    printf("before injection = %d\n", flag);

    if (flag){
        result = 100;
    } else {
        result = 200;
    }

    printf("after injection = %d\n", result);
    return 0;
}