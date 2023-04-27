#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h> // termios, TCSANOW, ECHO, ICANON
#include <unistd.h>
#include <sys/file.h>
#include <ctype.h>
#include <dirent.h>


const char *sysname = "mishell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count; // arg_count = name + ... + NULL
	char **args;
	char *redirects[3]; // in/out redirection  //0 read 1 write 2 append
	struct command_t *next; // for piping
};


/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n",
		   command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");

	for (i = 0; i < 3; i++) {
		printf("\t\t%d: %s\n", i,
			   command->redirects[i] ? command->redirects[i] : "N/A");
	}

	printf("\tArguments (%d):\n", command->arg_count);

	for (i = 0; i < command->arg_count; ++i) {
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	}

	if (command->next) {
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
	if (command->arg_count) {
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}

	for (int i = 0; i < 3; ++i) {
		if (command->redirects[i])
			free(command->redirects[i]);
	}

	if (command->next) {
		free_command(command->next);
		command->next = NULL;
	}

	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);

	// trim left whitespace
	while (len > 0 && strchr(splitters, buf[0]) != NULL) {
		buf++;
		len--;
	}

	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL) {
		// trim right whitespace
		buf[--len] = 0;
	}

	// auto-complete
	if (len > 0 && buf[len - 1] == '?') {
		command->auto_complete = true;
	}

	// background
	if (len > 0 && buf[len - 1] == '&') {
		command->background = true;
	}

	char *pch = strtok(buf, splitters);
	if (pch == NULL) {
		command->name = (char *)malloc(1);
		command->name[0] = 0;
	} else {
		command->name = (char *)malloc(strlen(pch) + 1);
		strcpy(command->name, pch);
	}

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1) {
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// trim left whitespace
		while (len > 0 && strchr(splitters, arg[0]) != NULL) {
			arg++;
			len--;
		}

		// trim right whitespace
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) {
			arg[--len] = 0;
		}

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// piping to another command
		if (strcmp(arg, "|") == 0) {
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0) {
			// handled before
			continue;
		}

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<') {
			redirect_index = 0; //read
		}

		if (arg[0] == '>') {
			if (len > 1 && arg[1] == '>') {
				redirect_index = 2; //append
				arg++;
				len--;
			} else {
				redirect_index = 1; //write
			}
		}

		if (redirect_index != -1) {
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 &&
			((arg[0] == '"' && arg[len - 1] == '"') ||
			 (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}

		command->args =
			(char **)realloc(command->args, sizeof(char *) * (arg_index + 1));

		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;

	// increase args size by 2
	command->args = (char **)realloc(
		command->args, sizeof(char *) * (command->arg_count += 2));

	// shift everything forward by 1
	for (int i = command->arg_count - 2; i > 0; --i) {
		command->args[i] = command->args[i - 1];
	}

	// set args[0] as a copy of name
	command->args[0] = strdup(command->name);

	// set args[arg_count-1] (last) to NULL
	command->args[command->arg_count - 1] = NULL;

	return 0;
}

void prompt_backspace() {
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
	size_t index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &=
		~(ICANON |
		  ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	show_prompt();
	buf[0] = 0;

	while (1) {
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		// handle tab
		if (c == 9) {
			buf[index++] = '?'; // autocomplete
			break;
		}

		// handle backspace
		if (c == 127) {
			if (index > 0) {
				prompt_backspace();
				index--;
			}
			continue;
		}

		if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
			continue;
		}

		// up arrow
		if (c == 65) {
			while (index > 0) {
				prompt_backspace();
				index--;
			}

			char tmpbuf[4096];
			printf("%s", oldbuf);
			strcpy(tmpbuf, buf);
			strcpy(buf, oldbuf);
			strcpy(oldbuf, tmpbuf);
			index += strlen(buf);
			continue;
		}

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}

	// trim newline from the end
	if (index > 0 && buf[index - 1] == '\n') {
		index--;
	}

	// null terminate string
	buf[index++] = '\0';

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);

int main() {
	while (1) {
		struct command_t *command = malloc(sizeof(struct command_t));

		// set all bytes to 0
		memset(command, 0, sizeof(struct command_t));

		int code;
		code = prompt(command);
		if (code == EXIT) {
			break;
		}

		code = process_command(command);
		if (code == EXIT) {
			break;
		}

		free_command(command);
	}

	printf("\n");
	return 0;
}

int find_executable(struct command_t *command) {
	char *pathvar = getenv("PATH");
	char *path = strtok(pathvar, " ");
	int len = strlen(path);
	for (int i = 0; i < len; i++) {
		if (access(path, X_OK) == 0) {
			command->args[0] = path;
			return SUCCESS;
		}

		return UNKNOWN;
	}
	return UNKNOWN;

}

int roll(char *input) {

	int number_of_rolls=0, lower=0, upper=0, sum=0;
	if (strlen(input) == 2 && input[0] == 'd' && isdigit(input[1])) {
		lower=1, upper=atoi(&input[1]), number_of_rolls=1;
	} else if (strlen(input) == 3 && input[1] == 'd' && isdigit(input[0]) && isdigit(input[2])) {
		lower=1, upper=atoi(&input[2]), number_of_rolls=atoi(&input[0]);
	} else {
		printf("Invalid input\n");
		return EXIT;
	}

	int *faces = malloc(number_of_rolls * sizeof(int));

	for (int i = 0 ; i < number_of_rolls ; i++) {
		int face = (rand() % (upper - lower + 1)) + lower;
		faces[i] = face;
		sum += face;
	}

	char pattern[1024] = {'\0'};

	if ((strlen(input) == 2)) {
		printf("Rolled %d\n", sum);
	} else {
		for (int i = 0; i < number_of_rolls; i++) {
			char num_str[10];
			sprintf(num_str, "%d", faces[i]);

			if (i == 0) {
				strcat(pattern, "(");
				strcat(pattern, num_str);
				strcat(pattern, " + ");
			} else if (i == number_of_rolls - 1) {
				strcat(pattern, num_str);
				strcat(pattern, ")");
			} else {
				strcat(pattern, num_str);
				strcat(pattern, " + ");
			}
		}
		printf("Rolled %d %s\n", sum, pattern);
	}

	free(faces);

	return SUCCESS;

}

//cdh part write to file from cd
void writefile(char* directory) {
	char *homedir = getenv("HOME");
	char path[1024];
	char path2[1024];
	if (homedir == NULL) {
		fprintf(stderr, "Error: no HOME env");
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", homedir, "cdh_history.txt");

	FILE *fp = fopen(path, "r+");
	if (fp == NULL) {
		// Try creating the file instead
		fp = fopen(path, "w");

		if (fp == NULL) {
			fprintf(stderr, "Cannot create or open file for appending: %s\n", strerror(errno));
			return;
		}

		fprintf(fp, "%s", directory);

	} else {
		snprintf(path2, sizeof(path2), "%s/%s", homedir, "temp.txt");
		FILE *temp = fopen(path2, "w");
		fprintf(temp, "%s\n", directory);

		char line[256];
		while (fgets(line, sizeof(line), fp) != NULL) {
			fprintf(temp, "%s\n", line);
		}

		fclose(temp);
		temp = fopen(path2, "r"); //temp file to update chd_history.txt
		fp = fopen(path, "w"); //overwrite the file

		while (fgets(line, sizeof(line), temp) != NULL) {
			if (strlen(line) != 1) { //do not write the newlines
				fprintf(fp, "%s", line);
			}
		}
		remove(path2); //remove the temp file after write
	}

	fclose(fp);
}

void cdh() {

	// Define variables
	FILE *file;
	char *homedir= getenv("HOME");
	char path[1024];

	// Check if HOME environment variable is set
	if (homedir == NULL) {
		fprintf(stderr, "Error: no HOME env");
		return;
	}

	// Construct path to history file
	snprintf(path, sizeof(path), "%s/%s", homedir, "cdh_history.txt");

	// Open history file for reading
	file = fopen(path, "r");

	if (file == NULL) {
		fprintf(stderr, "Error opening history file: %s\n", strerror(errno));
		return;
	}

	char lines[10][256];
	char line[256];
	int count = 0;
	while (fgets(line, sizeof(line), file) != NULL) {

		int found = 0; //1 if the element is printed before, 0 otherwise

		for (int i = 0; i < count; i++) {
			if (strcmp(line, lines[i]) == 0) {
				found = 1;
				break;
			}
		}
		if (strncmp(line, "..",2) == 0) {
			found = 1;
		} else if (strncmp(line, ".",1) == 0) {
            found = 1;
        }
		if (!found) {
			char alphabetic;
			alphabetic = 'a' + count;
            line[strcspn(line, "\n")] = '\0';
			strcpy(lines[count % 10], line);
			printf("%d) %c) %s\n", count+1, alphabetic, line);
			count++;
		}
		if (count >= 10) {
			break;
		}
	}

	fclose(file);

	char input[100];
	int num;
	char ch;
	char directory[512];

	printf("Select directory by letter or number: ");
	fgets(input, sizeof(input), stdin);
    input[strcspn(input, "\n")] = '\0';

    if (isdigit(input[0]) || isalpha(input[0])) {
        if (isdigit(input[0])) {
            num = atoi(input);
            ch = 'a' + (num - 1);
        } else {
            ch = input[0];
        }

        int ascii = ch;
        int index = ascii-97;

		if (ascii - 96 >= 1 && ascii - 96 <= count) {
			strcpy(directory, lines[index]);
            char* directoryPath;
            if (strcmp(&directory[0], "~") == 0 && strlen(directory) == 2) { //since ~ not supported in chdir, change it to home path
                directoryPath = malloc(sizeof(homedir) + 1);
                strcpy(directoryPath, homedir);
                chdir(directoryPath);
                free(directoryPath);
            } else if (strcmp(&directory[0],"~") == 0 && strcmp(&directory[1], "/") == 0) { //change the first index (~) of the string with home path
                char *ptr = directory;
                ptr++;
                memmove(directory, ptr, strlen(ptr) + 1);
                directoryPath = malloc(sizeof(homedir) + sizeof(directory) + 1);
                sprintf(directoryPath, "%s%s", homedir, directory);
                chdir(directoryPath);
                free(directoryPath);
            } else {
                directoryPath = malloc(sizeof(directory) + 1);
                strcpy(directoryPath, directory);
                chdir(directoryPath);
                free(directoryPath);
            }
		} else {
			printf("Invalid input\n");
		}
	}
}

//struct to hold the file values
struct Info {
    char name[20]; //to hold extension
    int files;
    int blank;
    int comment;
    int code;

};
//cloc command
void cloc_recursive_helper(char* drc, struct Info *c, struct Info *cpp, struct Info *h, struct Info *hpp, struct Info *python, struct Info *txt, struct Info *info ) {

    DIR *dir = opendir(drc);
    if (dir == NULL) {
        return;
    }

    struct dirent *entry;
    char path[1500];
    const char *extension;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue; // skip current and parent directory links
        }
        snprintf(path, sizeof(path), "%s/%s", drc, entry->d_name);

        if (entry->d_type == DT_DIR) {
            // recursively count lines in subdirectory
            cloc_recursive_helper(path, c, cpp, h, hpp, python, txt, info);
        } else if (entry->d_type == DT_REG) {
            extension = strrchr(entry->d_name, '.');
            if (extension == NULL) {
                extension = ".txt";
            }

            if (!(strcmp(extension, ".c") == 0 || strcmp(extension, ".h") == 0 || strcmp(extension, ".cpp") == 0 ||
                  strcmp(extension, ".hpp") == 0 || strcmp(extension, ".py") == 0 || strcmp(extension, ".txt") == 0)) {
                continue; //skip this iteration
            } else {
                //file operations
                FILE *file = fopen(path, "r");
                if (file == NULL) {
                    continue;
                }
                char line[1024];
                //to check if it is a comment or blank
                int comment = 0; //to check if code is in comment
                int blank = 0;
                int code = 0;

                while (1) {
                    if (fgets(line, 1024, file) == NULL) {
                        break;
                    }
                    int lc = 0;
                    //to increase line count if the char is blank or tab
                    while ((line[lc] == ' ') || (line[lc] == '\t')) {
                        lc++;
                    }

                    //check the blank lines
                    if (line[lc] == '\n' || line[lc] == '\r' || line[lc] == '\0') {
                        blank += 1;
                    } else {
                        if (strcmp(extension, ".py") == 0) {
                            if (line[lc] == '#') {
                                comment++;
                            } else {
                                code++;
                            }
                        } else {
                            //to check the comments
                            //for c and c++ inline comments
                            if (line[lc] == '/' && line[lc + 1] == '/') {
                                comment++;
                            } else if (line[lc] == '/' && line[lc + 1] == '*') {
                                comment++;
                            } else {
                                code++;
                            }
                        }
                    }
                }

                if (strcmp(extension, ".c") == 0) {
                    c->blank += blank;
                    c->comment += comment;
                    c->code += code;
                    c->files++;
                } else if (strcmp(extension, ".h") == 0) {
                    h->blank += blank;
                    h->comment += comment;
                    h->code += code;
                    h->files++;
                } else if (strcmp(extension, ".cpp") == 0) {
                    cpp->blank += blank;
                    cpp->comment += comment;
                    cpp->code += code;
                    cpp->files++;
                } else if (strcmp(extension, ".hpp") == 0) {
                    hpp->blank += blank;
                    hpp->comment += comment;
                    hpp->code += code;
                    hpp->files++;
                } else if (strcmp(extension, ".py") == 0) {
                    python->blank += blank;
                    python->comment += comment;
                    python->code += code;
                    python->files++;
                } else if (strcmp(extension, ".txt") == 0) {
                    txt->blank += blank;
                    txt->comment += comment;
                    txt->code += code;
                    txt->files++;
                }
                fclose(file);
                info->files++;
                info->blank += blank;
                info->comment += comment;
                info->code += code;
            }
        }
    }
}


void cloc(char *drc) {

    //initializing the structs
    struct Info *info = (struct Info *) malloc(sizeof(struct Info));
    strcpy(info->name, " ");
    info->files = 0;
    info->blank = 0;
    info->comment = 0;
    info->code = 0;

    struct Info *python = (struct Info *) malloc(sizeof(struct Info));
    strcpy(python->name, "Python");
    python->files = 0;
    python->blank = 0;
    python->comment = 0;
    python->code = 0;

    struct Info *c = (struct Info *) malloc(sizeof(struct Info));
    strcpy(c->name, "C");
    c->files = 0;
    c->blank = 0;
    c->comment = 0;
    c->code = 0;

    struct Info *cpp = (struct Info *) malloc(sizeof(struct Info));
    strcpy(cpp->name, "C++");
    cpp->files = 0;
    cpp->blank = 0;
    cpp->comment = 0;
    cpp->code = 0;

    struct Info *h = (struct Info *) malloc(sizeof(struct Info));
    strcpy(h->name, "C Header File");
    h->files = 0;
    h->blank = 0;
    h->comment = 0;
    h->code = 0;

    struct Info *hpp = (struct Info *) malloc(sizeof(struct Info));
    strcpy(hpp->name, "C++ Header File");
    hpp->files = 0;
    hpp->blank = 0;
    hpp->comment = 0;
    hpp->code = 0;

    struct Info *txt = (struct Info *) malloc(sizeof(struct Info));
    strcpy(txt->name, "Text");
    txt->files = 0;
    txt->blank = 0;
    txt->comment = 0;
    txt->code = 0;

    cloc_recursive_helper(drc, c, cpp, h, hpp, python, txt, info);

    //totals
    printf("Total number of files in the given directory: %d\n", info->files);
    printf("\n");
    printf("Total blank lines %d\n",info->blank);
    printf("Total comment lines %d\n",info->comment);
    printf("Total code lines %d\n",info->code);
    printf("\n");
    //headers
    printf("%-20s %-10s %-10s %-10s %-10s\n", "Language", "Files", "Blank", "Comment","Code");
    printf("%-20s %-10d %-10d %-10d %-10d\n", c->name, c->files, c->blank, c->comment,c->code);
    printf("%-20s %-10d %-10d %-10d %-10d\n", h->name, h->files, h->blank, h->comment,h->code);
    printf("%-20s %-10d %-10d %-10d %-10d\n", cpp->name, cpp->files, cpp->blank, cpp->comment,cpp->code);
    printf("%-20s %-10d %-10d %-10d %-10d\n", hpp->name, hpp->files, hpp->blank, hpp->comment,hpp->code);
    printf("%-20s %-10d %-10d %-10d %-10d\n", python->name, python->files, python->blank, python->comment,python->code);
    printf("%-20s %-10d %-10d %-10d %-10d\n", txt->name, txt->files, txt->blank, txt->comment,txt->code);
}

//custom command 1
void sandstorm() {
    char cmd[100];
    char *url=("https://www.youtube.com/watch?v=y6120QOlsfU");
#ifdef __APPLE__
    sprintf(cmd,"open %s", url);
#elif _WIN32
    sprintf(cmd,"start %s", url);

#else
    sprintf(cmd,"xdg-open %s", url);
#endif
    system(cmd);
}

//custom command 2
void fortune() {
    char fortunes[25][256] = {"You will encounter a coding bug so bizarre, you'll start to wonder if your computer is possessed by a mischievous spirit.\n", "Your future holds a plethora of keyboard shortcuts that will make you feel like a wizard of the digital realm.\n", "In your future, you will finally solve a programming problem that's been driving you crazy - just in time for it to become obsolete.\n",
                         "Your computer will crash at the most inconvenient time possible, reminding you that technology truly has a sense of humor.\n", "The code you write today will run perfectly - but only on the machine you wrote it on. Good luck :).\n", "\"In the near future, you will discover the joys of pointer arithmetic in C. Don't worry, it's not as painful as it sounds.\n",
                         "You will encounter a bug in your C code that will make you question the fundamental laws of computer science.\n", "Your mastery of C will impress even the most seasoned programmers, earning you the nickname 'C-sar' among your peers.\n", "Your code will compile without errors, but when you run it, you'll be greeted with a delightful surprise: a segfault!\n",
                         "You will spend hours debugging a single line of code in C, only to find that the problem was caused by a misplaced semicolon.\n", "In the near future, you will experience the joy of watching an operating system update progress bar move at an excruciatingly slow pace\n", "You will encounter a mysterious error message while working with your operating system, leaving you wondering if the Matrix has just glitched.\n",
                         "Your future holds a visit to the dreaded Blue Screen of Death. Don't worry, it happens to the best of us. \n"," You will discover a hidden Easter egg in your operating system that will make you question whether the developers have a sense of humor or not.\n","Your operating system will suddenly decide to update itself in the middle of an important task, leaving you with a newfound appreciation for manual updates.\n","You will encounter the Linux terminal for the first time and feel like you've been transported to a world of endless possibilities.\n",
                         "Your future holds a late-night session of compiling and installing packages from source code, leaving you feeling like a true Linux guru.\n", "You will experience the satisfaction of solving a complex problem using Linux command-line tools, and wonder how you ever lived without them.\n", "Your Linux system will crash unexpectedly, but fear not - with the power of the command line, you'll be able to diagnose and fix the issue in no time.\n", "You will discover the joys of customizing your Linux desktop environment, creating a unique setup that reflects your personality and style\n",
                         "You will become so proficient in Vim that you'll start editing text in your dreams with HJKL\n","In the future, you'll accidentally activate Vim's 'delete everything' mode and be left wondering if your document ever existed.\n","You'll become so comfortable using Vim that you'll start seeing regular text editors as mere toys.\n","You will encounter a fellow Vim user and bond over your mutual love for efficient editing and obscure keyboard shortcuts.\n","Your future holds a moment of panic when you realize you can't exit Vim, but fear not - Google and the Vim community will come to your rescue.\n"
    };

    int random_fortune = rand() % 25;
    printf("%s", fortunes[random_fortune]);
}

void psvis(pid_t pid) { //add output file

    struct ast_node* get_ast_for_pid(pid);

}

int process_command(struct command_t *command) {
	int r;

	if (strcmp(command->name, "") == 0) {
		return SUCCESS;
	}


	if (strcmp(command->name, "exit") == 0) {
		return EXIT;
	}

	if (strcmp(command->name, "cd") == 0) {
		if (command->arg_count > 0) {
			r = chdir(command->args[1]);
			writefile(command->args[1]);
			if (r == -1) {
				printf("-%s: %s: %s\n", sysname, command->name,
					   strerror(errno));
			}
			return SUCCESS;
		}
	}

	if (strcmp(command->name, "roll") == 0) {
		roll(command->args[1]);
		return SUCCESS;
	}

	if (strcmp(command->name, "cdh") == 0) {
		cdh();
		return SUCCESS;
	}

    if (strcmp(command->name, "cloc") == 0) {
        cloc(command->args[1]);
        return SUCCESS;
    }

    if (strcmp(command->name, "sandstorm") == 0) {
        sandstorm();
        return SUCCESS;
    }

    if (strcmp(command->name, "fortune") == 0) {
        fortune();
        return SUCCESS;
    }

    if (strcmp(command->name, "psvis") == 0) {
        if (command->arg_count > 0) {
            //psvis(atoi(command->args[1]));
        }
        return SUCCESS;
    }

	//if not found
	if (find_executable(command) == UNKNOWN) {
		printf("-%s: %s: command not found\n", sysname, command->name);
		return UNKNOWN;
	}

	pid_t pid = fork();

	//redirection

	//input file redirection (read)
	int fd;
	if (command->redirects[0]!= NULL) {
		fd = open(command->redirects[0], O_RDONLY);
		if (fd == -1) {
			perror("open");
			exit(EXIT_FAILURE);
		}

	}

	//output file redirection

	int fd1;
	int fd2;

	//for write
	if (command->redirects[1]!= NULL) {
		fd1 = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd1 == -1) {
			perror("open");
			exit(EXIT_FAILURE);
		}
	}
	//for append
	if (command->redirects[2]!= NULL) {
		fd2 = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd2 == -1) {
			perror("open");
			exit(EXIT_FAILURE);
		}
	}

	// child
	if (pid == 0) {


        printf("%d\n",getpid());
		/// This shows how to do exec with environ (but is not available on MacOs)
		// extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// TODO: do your own exec with path resolving using execv()
		// do so by replacing the execvp call below
		//char *cmd[] = {"/usr/bin/ls", "-l", NULL};

		execv(command->args[0], command->args); // exec+args+path
		exit(0);
	} else {
        printf("child");

        wait(NULL);
	}

    //to reach childs of the parent

	if (!command->background) {
		wait(0);
	}

	//duplicating and closing read file
	if (command->redirects[0]!= NULL) {
		if (dup2(fd, STDIN_FILENO) == -1) {
			perror("dup2");
			exit(EXIT_FAILURE);
		}
		if (close(fd) == -1) {
			perror("close");
			exit(EXIT_FAILURE);
		}
	}

	//duplicating and closing write file

	if (command->redirects[0]!= NULL) {
		if (dup2(fd1, STDOUT_FILENO) == -1) {
			perror("dup2");
			exit(EXIT_FAILURE);
		}
		if (close(fd1) == -1) {
			perror("close");
			exit(EXIT_FAILURE);
		}
	}

	//duplicating and closing append file
	if (command->redirects[0]!= NULL) {
		if (dup2(fd2, STDOUT_FILENO) == -1) {
			perror("dup2");
			exit(EXIT_FAILURE);
		}
		if (close(fd2) == -1) {
			perror("close");
			exit(EXIT_FAILURE);
		}
	}

	return SUCCESS;

}
