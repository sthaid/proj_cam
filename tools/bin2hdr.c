// this programs converts binary data to c header file
//
// usage:   bin2hdr < binary > header.h

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_BUFF 10000000

uint8_t buff[MAX_BUFF];
int     buff_len;

int main()
{
    int len, i;

    // read stdin until eod
    while (true) {
        // read data
        len = read(0, buff+buff_len, MAX_BUFF-buff_len);
        if (len < 0) {
            printf("ERROR: failed read from stdin\n");
            return 1;
        }

        // check for eod
        if (len == 0) {
            break;
        }

        // keep track of total len data read
        buff_len += len;

        // check for more data than this program supports
        if (buff_len >= MAX_BUFF) {
            printf("ERROR: input buffer too big\n");
            return 1;
        }
    }

    // write data in header file format
    printf("uint8_t buff[%d] = {\n", buff_len);
    for (i = 0; i < buff_len; i++) {
        // print offset
        if ((i % 16) == 0) {
            printf("    /* %5d */ ", i);
        }

        // print data
        printf("0x%2.2x, ", buff[i]);

        // check for done
        if (i == buff_len - 1) {
            printf("};\n");
            break;
        }

        // check for end of line
        if ((i % 16) == 15) {
            printf("\n");
        }
    }

    // done
    return 0;
}

