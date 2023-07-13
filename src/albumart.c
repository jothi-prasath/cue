#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <dirent.h>
#include "file.h"
#include "../include/imgtotxt/options.h"
#include "metadata.h"
#include "stringfunc.h"
#include "term.h"
#include "albumart.h"
#include "cache.h"
#include "chafafunc.h"

FIBITMAP *bitmap;

int extractCoverCommand(const char *inputFilePath, const char *outputFilePath)
{
    const int COMMAND_SIZE = 1000;
    char command[COMMAND_SIZE];
    int status;

    // Replace $ with \$
    char *escapedInputFilePath = escapeFilePath(inputFilePath);
    char *escapedOutputFilePath = escapeFilePath(outputFilePath);

    snprintf(command, COMMAND_SIZE, "ffmpeg -y -i \"%s\" -an -vcodec copy \"%s\"", escapedInputFilePath, escapedOutputFilePath);

    free(escapedInputFilePath);
    free(escapedOutputFilePath);

    pid_t pid = fork();

    if (pid == -1)
    {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    else if (pid == 0)
    {
        // Child process
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        exit(EXIT_SUCCESS);
    }
    else
    {
        // Parent process
        waitpid(pid, &status, 0);
    }

    FILE *file = fopen(outputFilePath, "r");

    if (file != NULL)
    {
        fclose(file);
        return 1;
    }
    else
    {
        return -1;
    }
}

int isAudioFile(const char *filename)
{
    const char *extensions[] = {".mp3", ".wav", ".m4a", ".flac", ".ogg"};

    for (long unsigned int i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    {
        if (strstr(filename, extensions[i]) != NULL)
        {
            return 1;
        }
    }
    return 0;
}

void addSlashIfNeeded(char *str)
{
    size_t len = strlen(str);

    if (len > 0 && str[len - 1] != '/')
    {
        strcat(str, "/");
    }
}

int compareEntries(const struct dirent **a, const struct dirent **b)
{
    const char *nameA = (*a)->d_name;
    const char *nameB = (*b)->d_name;

    if (nameA[0] == '_' && nameB[0] != '_')
    {
        return -1; // Directory A starts with underscore, so it should come first
    }
    else if (nameA[0] != '_' && nameB[0] == '_')
    {
        return 1; // Directory B starts with underscore, so it should come first
    }

    return strcmp(nameA, nameB); // Lexicographic comparison for other cases
}

char *findFirstPathWithAudioFile(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        return NULL;
    }

    struct dirent **entries;
    int numEntries = scandir(path, &entries, NULL, compareEntries);

    if (numEntries < 0)
    {
        closedir(dir);
        return NULL;
    }

    char *audioDirectory = NULL;
    for (int i = 0; i < numEntries; i++)
    {
        struct dirent *entry = entries[i];
        if (S_ISDIR(entry->d_type))
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }
            char subDirectoryPath[MAXPATHLEN];
            snprintf(subDirectoryPath, MAXPATHLEN, "%s/%s", path, entry->d_name);

            audioDirectory = findFirstPathWithAudioFile(subDirectoryPath);
            if (audioDirectory != NULL)
            {
                break;
            }
        }
        else
        {
            if (isAudioFile(entry->d_name))
            {
                audioDirectory = strdup(path);
                break;
            }
        }
    }

    for (int i = 0; i < numEntries; i++)
    {
        free(entries[i]);
    }
    free(entries);
    closedir(dir);
    return audioDirectory;
}

char *findLargestImageFileRecursive(const char *directoryPath, char *largestImageFile, off_t *largestFileSize)
{
    DIR *directory = opendir(directoryPath);
    struct dirent *entry;
    struct stat fileStats;

    if (directory == NULL)
    {
        fprintf(stderr, "Failed to open directory: %s\n", directoryPath);
        return largestImageFile;
    }

    while ((entry = readdir(directory)) != NULL)
    {
        char filePath[MAXPATHLEN];
        snprintf(filePath, MAXPATHLEN, "%s/%s", directoryPath, entry->d_name);

        if (stat(filePath, &fileStats) == -1)
        {
            // fprintf(stderr, "Failed to get file stats: %s\n", filePath);
            continue;
        }

        if (S_ISDIR(fileStats.st_mode))
        {
            // Skip "." and ".." directories
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            // Recursive call to process subdirectories
            largestImageFile = findLargestImageFileRecursive(filePath, largestImageFile, largestFileSize);
        }
        else if (S_ISREG(fileStats.st_mode))
        {
            // Check if the entry is an image file and has a larger size than the current largest image file
            char *extension = strrchr(entry->d_name, '.');
            if (extension != NULL && (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0 ||
                                      strcasecmp(extension, ".png") == 0 || strcasecmp(extension, ".gif") == 0))
            {
                if (fileStats.st_size > *largestFileSize)
                {
                    *largestFileSize = fileStats.st_size;
                    if (largestImageFile != NULL)
                    {
                        free(largestImageFile);
                    }
                    largestImageFile = strdup(filePath);
                }
            }
        }
    }

    closedir(directory);
    return largestImageFile;
}

int calcIdealImgSize(int *width, int *height, const int visualizerHeight, const int metatagHeight)
{
    int term_w, term_h;
    getTermSize(&term_w, &term_h);
    int timeDisplayHeight = 1;
    int heightMargin = 2;
    int minHeight = visualizerHeight + metatagHeight + timeDisplayHeight + heightMargin;
    *height = term_h - minHeight;
    *width = ceil(*height * 2);
    if (*width > term_w)
    {
        *width = term_w;
        *height = floor(*width / 2);
    }
    int remainder = *width % 2;
    if (remainder == 1)
        *width -= 1;
    return 0;
}

int displayCover(SongData *songdata, int width, int height, bool ansii)
{
    if (!ansii)
    {
        clearScreen();
        printBitmapCentered(songdata->cover, width - 1, height - 2);
    }
    else
    {
        cursorJump(1);
        PixelData pixel = {*songdata->red, *songdata->green, *songdata->blue};
        output_ascii(songdata->coverArtPath, height - 2, width - 1, &pixel);
    }
    fputc('\n', stdout);
    return 0;
}