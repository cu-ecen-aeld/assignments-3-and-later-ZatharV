/**
 * writer.c
 * 
 * Author: Atharv More
 * 
 */


#include "stdio.h"
#include "syslog.h"

/**
 * Accepts 2 arguments:
 * 1. The name of the file to create
 * 2. String to write in the created file
 */
int main(int argc, char **argv)
{
    openlog("writer", 0, LOG_USER);

    // First arg is this file
    if(argc != 3)
    {
        syslog(LOG_ERR, "You should pass 2 arguments. The order of arguments should be 'writefile' and 'writestr'.");
        closelog();
        return 1;
    }

    char* filename = argv[1];   // Argument 1 is the file name
    char* string = argv[2];     // Argument 2 is the string

    // Creates new or override existing file
    FILE* file = fopen(filename, "w");

    if(file == NULL)
    {
        syslog(LOG_ERR, "Error creating the file.");
        closelog();
        return 1;
    }

    // Writes string in the file
    syslog(LOG_DEBUG, "Writing %s to %s.", string, filename);
    fprintf(file, "%s", string);

    fclose(file);   // Close the file

    closelog();
    return 0;
}