#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <strings.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <algorithm>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <regex>

using namespace std;

#define MAX_BUF_SIZE    15000
#define CONTENT_SIZE    1025
#define BUILT_IN_EXIT   99
#define BUILT_IN_TRUE   1
#define BUILT_IN_FALSE  0
#define USER_LIMIT      30
#define USERSHMKEY ((key_t) 7890)
#define MSGSHMKEY  ((key_t) 7891)

typedef struct mypipe {
    int in;
    int out;
} Pipe;

typedef struct my_number_pipe {
    int in;
    int out;
    int number;
} NumberPipe;

typedef struct my_command {
    string cmd;           // Cut by number|error pipe
    vector<string> cmds;  // Split by pipe
    int number;           // For number pipe
    int in_fd, out_fd;    // For user pipe
} Command;

typedef struct my_user {
    int uid;
    int sockfd;
    pid_t pid;
    bool is_active;
    char name[32];
    char ip_addr[INET6_ADDRSTRLEN];
} User;

typedef struct my_message {
    int length;
    int counter;
    bool is_active;
    char content[CONTENT_SIZE];
} Message;

typedef struct user_context {
    string original_input;
    map<string, string> env;
    vector<Pipe> pipes;
    vector<NumberPipe> number_pipes;
} Context;

/* Global Value */
int user_shm_id, msg_shm_id;
User *user_shm_ptr;
Message *msg_shm_ptr;
int listen_sock;
pthread_mutex_t user_mutex, msg_mutex;

/* Function Prototype */
int get_online_user_number();
void sendout_msg(int sockfd, string &msg);
void logout_prompt(int uid);
/* Function Prototype End */

void init_shm() {
    void *tmp_ptr;

    // Get user shared memory ID
    user_shm_id = shmget(USERSHMKEY, sizeof(User) * USER_LIMIT, IPC_CREAT | IPC_EXCL | SHM_R | SHM_W);
    if (user_shm_id < 0) {
        perror("Get user shm");
        exit(0);
    }
    // Attach user shared memory
    tmp_ptr = shmat(user_shm_id, NULL, 0);
    if (*((int*) tmp_ptr) == -1) {
        perror("Map user shm");
        exit(0);
    }
    user_shm_ptr = static_cast<User *>(tmp_ptr);
    bzero((char *)user_shm_ptr, sizeof(User) * USER_LIMIT);

    #if 0
    for(int x=0; x < USER_LIMIT; ++x) {
        if (!user_shm_ptr[x].is_active) {
            cout << "UID: " << x+1 << " is not active" << endl;
        }
    }
    #endif

    // Get message shared memory ID
    msg_shm_id = shmget(MSGSHMKEY, sizeof(Message) * 1, IPC_CREAT | IPC_EXCL | SHM_R | SHM_W);
    if (msg_shm_id < 0) {
        perror("Get msg shm");
        exit(0);
    }
    // Attach message shared memory
    tmp_ptr = shmat(msg_shm_id, NULL, 0);
    if (*((int*) tmp_ptr) == -1) {
        perror("Map msg shm");
        exit(0);
    }
    msg_shm_ptr = static_cast<Message *>(tmp_ptr);
    bzero((char *)msg_shm_ptr, sizeof(Message) * 1);

    return;
}

void init_lock() {
    pthread_mutexattr_t user_mutex_attr;
    pthread_mutexattr_t msg_mutex_attr;

    pthread_mutexattr_init(&user_mutex_attr);
    pthread_mutexattr_setpshared(&user_mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&user_mutex, &user_mutex_attr);

    pthread_mutexattr_init(&msg_mutex_attr);
    pthread_mutexattr_setpshared(&msg_mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&msg_mutex, &msg_mutex_attr);
}

/* Server Related*/
void server_exit() {
    cout << "Wait all user leave..." << endl;
    while (true) {
        int counter = get_online_user_number();
        if (counter == 0) {
            break;
        } else {
            cout << "Remain " << counter << " users online" << endl;
            usleep(100);
        }
    }

    cout << "Detach and Remove Shared Memory" << endl;
    if (shmdt(user_shm_ptr) < 0) {
        perror("Detach user shm");
    }
    if (shmctl(user_shm_id, IPC_RMID, NULL) < 0) {
        perror("Remove user shared memory");
    }
    if (shmdt(msg_shm_ptr) < 0) {
        perror("Detach message shm");
    }
    if (shmctl(msg_shm_id, IPC_RMID, NULL) < 0) {
        perror("Remove message shared memory");
    }
    cout << "Close listen socket" << endl;
    close(listen_sock);


    exit(0);
}

void signal_server_handler(int sig) {
    if (sig == SIGCHLD) {
		while(waitpid (-1, NULL, WNOHANG) > 0);
	} else if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM){
        server_exit();
	}
}

bool is_user_up_to_limit() {
    for (int x=0; x < USER_LIMIT; ++x) {
        if (!user_shm_ptr[x].is_active) {
            return false;
        }
    }
    return true;
}

int get_listen_socket(const char *port) {
    struct sockaddr_in s_addr;
    int listen_sock;
    int status_code;

    // Init variable
    bzero((char *)&s_addr, sizeof(s_addr));

    // Init server address
    s_addr.sin_family = AF_INET;
    s_addr.sin_addr.s_addr = INADDR_ANY;
    s_addr.sin_port = htons((u_short)atoi(port));

    // Create socket
    listen_sock = socket(s_addr.sin_family, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("Server create socket");
        exit(0);
    }

    // Socket setting
    int optval = 1;
    status_code = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    if (status_code < 0) {
        perror("Set socket option");
        exit(0);
    }

    // Bind socket
    status_code = bind(listen_sock, (struct sockaddr *) &s_addr, sizeof(s_addr));
    if (status_code < 0) {
        perror("Server bind");
        exit(0);
    }

    // Listen socket
    status_code = listen(listen_sock, 5);
    if (status_code < 0) {
        perror("Server listen");
        exit(0);
    }

    return listen_sock;
}

int get_online_user_number() {
    int counter = 0;

    pthread_mutex_lock(&user_mutex);
    for(int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active) {
            ++counter;
        }
    }
    pthread_mutex_unlock(&user_mutex);

    return counter;
}

int get_sockfd_by_pid(pid_t pid) {
    int result = -1;

    for(int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active && user_shm_ptr[x].pid == pid) {
            result = user_shm_ptr[x].sockfd;
            break;
        }
    }

    return result;
}

int get_uid_by_pid(pid_t pid) {
    int result = -1;

    for(int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active && user_shm_ptr[x].pid == pid) {
            result = user_shm_ptr[x].uid;
            break;
        }
    }

    return result;
}

bool has_user(int target_uid) {
    bool result = false;

    for(int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active && user_shm_ptr[x].uid == target_uid) {
            result = true;
            break;
        }
    }

    return result;
}
/* Server Related End*/

/* User Related */
void debug_user() {
    cout << "***** Debug user start" << endl;
    for (size_t i = 0; i < USER_LIMIT; i++) {
        if (user_shm_ptr[i].is_active) {
            printf("User (%d):\tname: %s\n\tpid: %d\n\tsockfd: %d\n\tip: %s\n",
                user_shm_ptr[i].uid,
                user_shm_ptr[i].name,
                user_shm_ptr[i].pid,
                user_shm_ptr[i].sockfd,
                user_shm_ptr[i].ip_addr
            );
        }
    }
    cout << "***** Debug user end" << endl;
}

int create_user(int sock, sockaddr_in addr) {
    // Invoked in child process
    int uid;
    char ip[INET6_ADDRSTRLEN];
    sprintf(ip, "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    pthread_mutex_lock(&user_mutex);
    for (uid=1; uid <= USER_LIMIT; ++uid) {
        if(user_shm_ptr[uid-1].is_active == false) {
            user_shm_ptr[uid-1].uid = uid;
            user_shm_ptr[uid-1].pid = getpid();
            user_shm_ptr[uid-1].is_active = true;
            user_shm_ptr[uid-1].sockfd = sock;
            strcpy(user_shm_ptr[uid-1].name, "(no name)");
            strncpy(user_shm_ptr[uid-1].ip_addr, ip, INET6_ADDRSTRLEN);

            break;
        }
    }
    pthread_mutex_unlock(&user_mutex);

    return uid;
}

void user_exit_procedure(int uid) {
    logout_prompt(uid);
    close(user_shm_ptr[uid-1].sockfd);
    bzero(&(user_shm_ptr[uid-1]), sizeof(User));
    exit(0);
}

void signal_child_handler(int sig) {
    char buf[CONTENT_SIZE];
    bzero(buf, CONTENT_SIZE);

    if (sig == SIGUSR1) {
        // Receive boradcast message
        while (true) {
            pthread_mutex_lock(&msg_mutex);
            strncpy(buf, msg_shm_ptr->content, msg_shm_ptr->length);
            msg_shm_ptr->counter--;

            if (msg_shm_ptr->counter <= 0) {
                msg_shm_ptr->is_active = false;
            }
            pthread_mutex_unlock(&msg_mutex);
            break;
        }

        string msg(buf);
        sendout_msg(get_sockfd_by_pid(getpid()), msg);
    } else if (sig == SIGUSR2) {
        // Receive user pipe
	} else if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM){
        user_exit_procedure(get_uid_by_pid(getpid()));
    }


}
/* User Related End*/

/* Network IO */
string read_msg(int sockfd) {
    static int cmd_counter = 0;
    char buf[MAX_BUF_SIZE];
    bzero(buf, MAX_BUF_SIZE);

    if (read(sockfd, buf, MAX_BUF_SIZE) < 0) {
        perror("Read Message.");
        return "";
    }

    string msg(buf);
    msg.erase(remove(msg.begin(), msg.end(), '\n'), msg.end());
    msg.erase(remove(msg.begin(), msg.end(), '\r'), msg.end());
    #if 1
    ++cmd_counter;
    printf("(%d) Recv (%ld): %s\n", cmd_counter, msg.length(), msg.c_str());
    #endif

    return msg;
}

void sendout_msg(int sockfd, string &msg) {
    int n = write(sockfd, msg.c_str(), msg.length());
    if (n < 0) {
        perror("Sendout Message");
        exit(0);
    }
}
/* Network IO End */

bool is_white_char(string cmd) {
    for (size_t i = 0; i < cmd.length(); i++) {
        if(isspace(cmd[i]) == 0) {
            return false;
        }
    }
    return true;
}

void broadcast(string msg) {
    while (true) {
        pthread_mutex_lock(&msg_mutex);
        if (msg_shm_ptr->is_active) {
            pthread_mutex_unlock(&msg_mutex);
            usleep(100);
            continue;
        } else {
            msg_shm_ptr->is_active = true;
            msg_shm_ptr->counter = get_online_user_number();
            msg_shm_ptr->length = msg.length();
            strncpy(msg_shm_ptr->content, msg.c_str(), msg.length());
            pthread_mutex_unlock(&msg_mutex);
            break;
        }
    }

    for (int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active) {
            kill(user_shm_ptr[x].pid, SIGUSR1);
        }
    }
    
    return;
}

void login_prompt(int uid) {
    ostringstream oss;
    string msg;

    // Create message
    oss << "*** User '" << user_shm_ptr[uid-1].name << "' entered from " << user_shm_ptr[uid-1].ip_addr << ". ***" << endl;
    msg = oss.str();

    // Broadcast
    broadcast(msg);
}

void logout_prompt(int uid) {
    ostringstream oss;
    string msg;

    // Create message
    oss << "*** User '" << user_shm_ptr[uid-1].name << "' left. ***" << endl;
    msg = oss.str();

    // Broadcast
    broadcast(msg);
}

void command_prompt(int uid) {
    string msg("% ");
    sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
}

void welcome(int uid) {
    ostringstream oss;
    string msg;

    oss << "****************************************" << endl
        << "** Welcome to the information server. **" << endl
        << "****************************************" << endl;
    msg = oss.str();

    sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
}

void who(int uid) {
    ostringstream oss;
    string tab("\t"), is_me("<-me"), msg;

    // Create Message
    oss << "<ID>\t<nickname>\t<IP:port>\t<indicate me>" << endl;
    for (int x=0; x < USER_LIMIT; ++x) {
        if (user_shm_ptr[x].is_active) {
            oss << user_shm_ptr[x].uid << tab
                << user_shm_ptr[x].name << tab
                << user_shm_ptr[x].ip_addr << tab
                << ((user_shm_ptr[x].uid == uid) ? is_me : "")
                << endl;
        }
    }
    msg = oss.str();

    // Send out Message
    sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
}

void tell(int uid, int tid, string msg) {
    ostringstream oss;

    if (has_user(tid)) {
        // Create message
        oss << "*** " << user_shm_ptr[uid-1].name << " told you ***: " << msg << endl;
        msg = oss.str();

        // Use message shared memory
        pthread_mutex_lock(&msg_mutex);
        msg_shm_ptr->is_active = true;
        msg_shm_ptr->counter = 1;
        msg_shm_ptr->length = msg.length();
        strncpy(msg_shm_ptr->content, msg.c_str(), msg.length());
        pthread_mutex_unlock(&msg_mutex);

        // Send signal
        kill(user_shm_ptr[tid-1].pid, SIGUSR1);
    } else {
        // User is not exist
        oss << "*** Error: user #" << tid << " does not exist yet. ***" << endl;
        msg = oss.str();

        sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
    }
}

void yell(int uid, string msg) {
    ostringstream oss;
    
    // Create message
    oss << "*** " << user_shm_ptr[uid-1].name << " yelled ***: " << msg << endl;
    msg = oss.str();

    // Broadcast message
    broadcast(msg);
}

void name_cmd(int uid, string name) {
    ostringstream oss;
    string msg;

    // Check name
    for (int x=0; x < USER_LIMIT; ++x) {
        if (name.compare(user_shm_ptr[x].name) == 0) {
            // The name is already exist
            oss << "*** User '" << name << "' already exists. ***" << endl;
            msg = oss.str();
            sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
            return;
        }
    }

    // Change name and broadcast message
    pthread_mutex_lock(&user_mutex);
    bzero(user_shm_ptr[uid-1].name, 32);
    strncpy(user_shm_ptr[uid-1].name, name.c_str(), name.length());
    pthread_mutex_unlock(&user_mutex);
    oss << "*** User from " << user_shm_ptr[uid-1].ip_addr << " is named '" << name << "'. ***" << endl;
    msg = oss.str();

    broadcast(msg);
}

void decrement_and_cleanup_number_pipes(vector<NumberPipe> &number_pipes) {
    vector<int> index;

    for (size_t i = 0; i < number_pipes.size(); i++) {
        if( --number_pipes[i].number < -1 ) index.push_back(i);
    }
    for (int i = index.size()-1; i >= 0; --i) {
        number_pipes.erase(number_pipes.begin() + index[i]);
    }
}

vector<string> parse_normal_pipe(string input) {
    vector<string> cmds;
    string pipe_symbol("| ");
    int pos;

    // Parse pipe
    while ((pos = input.find(pipe_symbol)) != string::npos) {
        cmds.push_back(input.substr(0, pos));
        input.erase(0, pos + pipe_symbol.length());
    }
    cmds.push_back(input);

    // Remove space
    for (size_t i = 0; i < cmds.size(); i++) {
        int head = 0, tail = cmds[i].length() - 1;

        while(cmds[i][head] == ' ') ++head;
        while(cmds[i][tail] == ' ') --tail;

        if (head != 0 || tail != cmds[i].length() - 1) {
            cmds[i] = cmds[i].substr(head, tail + 1);
        }
    }

    return cmds;
}

vector<Command> parse_number_pipe(string input) {
    /*
     *    "removetag test.html |2 ls | number |1"
     * -> ["removetag test.html |2", "ls | number |1"]
     */
    vector<Command> lines;
    regex pattern("[|!][1-9]\\d?\\d?[0]?");
    smatch result;

    while(regex_search(input, result, pattern)) {
        Command command;
        string tmp;
        tmp = input.substr(0, result.position() + result.length());

        command.cmd = input.substr(0, result.position() + result.length());
        command.number = atoi(input.substr(result.position() + 1, result.length() - 1).c_str());
        input.erase(0, result.position() + result.length());

        lines.push_back(command);
    }

    if (input.length() != 0) {
        Command command{cmd: input};
        
        lines.push_back(command);
    }

    if (lines.size() == 0) {
        // Normal Pipe
        Command command{cmd: input};

        lines.push_back(command);
    }

    // Split by pipe
    for (size_t i = 0; i < lines.size(); i++) {
        lines[i].cmds = parse_normal_pipe(lines[i].cmd); 
    }

    // Debug
    #if 0
    for (size_t i = 0; i < lines.size(); i++) {
        cerr << "Line " << i << ": " << lines[i].cmd << endl;
        cerr << "\tCommand: " << lines[i].cmd << endl;
        cerr << "\tCommand Size: " << lines[i].cmds.size() << endl;
        cerr << "\tNumber: " << lines[i].number << endl;
    }
    #endif
        
    return lines;
}

int handle_builtin(int uid, string cmd, Context *context) {
    istringstream iss(cmd);
    string prog;

    getline(iss, prog, ' ');

    if (prog == "setenv") {
        string var, value;

        getline(iss, var, ' ');
        getline(iss, value, ' ');
        setenv(var.c_str(), value.c_str(), 1);
        context->env[var] = value;

        return BUILT_IN_TRUE;
    } else if (prog == "printenv") {
        string var;

        getline(iss, var, ' ');
        char *value = getenv(var.c_str());
    
        if (value) {
            ostringstream oss;
            string msg(value);

            oss << value << endl;
            msg = oss.str();

            sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
        }

        return BUILT_IN_TRUE;
    } else if (prog == "exit") {
        user_exit_procedure(uid);

        return BUILT_IN_EXIT;
    } else if (prog == "who") {
        who(uid);

        return BUILT_IN_TRUE;
    } else if (prog == "tell") {
        string tid, msg;

        getline(iss, tid, ' ');
        getline(iss, msg);

        tell(uid, atoi(tid.c_str()), msg);

        return BUILT_IN_TRUE;
    } else if (prog == "yell") {
        string msg;

        getline(iss, msg);
        yell(uid, msg);

        return BUILT_IN_TRUE;
    } else if (prog == "name") {
        string new_name;
        
        getline(iss, new_name, ' ');
        name_cmd(uid, new_name);

        return BUILT_IN_TRUE;
    }

    return BUILT_IN_FALSE;
}
// Built-in Command End

void execute_command(int uid, vector<string> args) {
    int fd;
    bool need_file_redirection = false;
    const char *prog = args[0].c_str();
    const char **c_args = new const char* [args.size()+1];  // Reserve one location for NULL

    #if 0
    cerr << "Execute Command Args: ";
    for (size_t i = 0; i < args.size(); i++) {
        cerr << args[i] << " ";
    }
    cerr << endl;
    #endif

    /* Parse Arguments */
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == ">") {
            need_file_redirection = true;
            fd = open(args[i+1].c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
            args.pop_back();  // Remove file name
            args.pop_back();  // Remove redirect symbol
            break;
        }
        c_args[i] = args[i].c_str();
    }
    c_args[args.size()] = NULL;

    // Check if need file redirection
    if (need_file_redirection) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    // Execute Command
    if (execvp(prog, (char **)c_args) == -1) {
        ostringstream oss;
        string msg;

        oss << "Unknown command: [" << args[0] << "]." << endl;
        msg = oss.str();
        sendout_msg(user_shm_ptr[uid-1].sockfd, msg);
        exit(1);
    }
}

int main_executor(int uid, Command &command, Context *context) {
    /* Pre-Process */
    decrement_and_cleanup_number_pipes(context->number_pipes);
    if (command.cmds.size() == 1) {
        int code = handle_builtin(uid, command.cmds[0], context);
        if (code != BUILT_IN_FALSE) {
            return code;
        }
    }
    #if 0
    cerr << "Handle " << command.cmd << endl;
    #endif

    vector<string> args;
    string error_pipe_symbol = "!", pipe_symbol = "|";
    string arg;
    bool is_error_pipe = false, is_number_pipe = false;
    bool is_input_user_pipe = false, is_output_user_pipe = false;
    bool is_input_user_pipe_error = false, is_output_user_pipe_error = false;
    
    // Handle a command per loop
    for (size_t i = 0; i < command.cmds.size(); i++) {
        istringstream iss(command.cmds[i]);
        vector<string> args;
        pid_t pid;
        int pipefd[2];
        int input_user_pipe_idx  = -1;
        int output_user_pipe_idx = -1;
        bool is_first_cmd = false, is_final_cmd = false;
        smatch in_result, out_result;

        if (i == 0)                        is_first_cmd = true;
        if (i == command.cmds.size() - 1)  is_final_cmd = true;

        // Check if there are user pipes
        // TODO: user pipe

        /* Parse Command to Args */
        #if 0
        cerr << "*** Parse Command: " << command.cmds[i] << endl;
        #endif

        while (getline(iss, arg, ' ')) {
            bool ignore_arg = false;

            if (is_white_char(arg)) continue;

            // if (regex_search(arg, in_result, up_in_pattern)) {
            //     ignore_arg = true;
            //     is_input_user_pipe_error = handle_input_user_pipe(me, arg, &input_user_pipe_idx);
            // }

            // if (regex_search(arg, out_result, up_out_pattern)) {
            //     ignore_arg = true;
            //     is_output_user_pipe_error = handle_output_user_pipe(me, arg, &output_user_pipe_idx);
            //     #if 0
            //     debug_user_pipes();
            //     #endif
            // }

            // Handle number and error pipe
            if (is_final_cmd) {
                if ((is_number_pipe = (arg.find("|") != string::npos)) ||
                    (is_error_pipe  = (arg.find("!") != string::npos))) {
                    bool is_add = false;
                    ignore_arg = true;

                    for (int x=0; x < context->number_pipes.size(); ++x) {
                        // Same number means that using the same pipe
                        if (context->number_pipes[x].number == command.number) {
                            context->number_pipes.push_back(NumberPipe{
                                in:     context->number_pipes[x].in,
                                out:    context->number_pipes[x].out,
                                number: command.number
                            });
                            is_add = true;
                            break;
                        }
                    }
                    // No match, Create a new pipe
                    if (!is_add) {
                        pipe(pipefd);
                        context->number_pipes.push_back(NumberPipe{
                            in: pipefd[0],
                            out: pipefd[1],
                            number: command.number});
                    }
                    #if 0
                    debug_number_pipes(context->number_pipes);
                    #endif
                }
            }

            // Ignore |N or !N
            if (!ignore_arg) {
                args.push_back(arg);
            }
        }
        /* Parse Command to Args End */

        /* Create Normal Pipe */
        if (!is_error_pipe && !is_number_pipe && !is_output_user_pipe) {
            if(!is_final_cmd && command.cmds.size() > 1) {
                pipe(pipefd);
                context->pipes.push_back(Pipe{in: pipefd[0], out: pipefd[1]});
                #if 0
                debug_pipes(context->pipes);
                #endif
            }
        }

        #if 0
        cerr << "User Pipe Flag:" << endl;
        cerr << "\tin: "      << (is_input_user_pipe ? "True" : "False") << endl
             << "\tout: "     << (is_output_user_pipe ? "True" : "False") << endl
             << "\tin err: "  << (is_input_user_pipe_error ? "True" : "False") << endl
             << "\tout err: " << (is_output_user_pipe_error ? "True" : "False") << endl;
        cerr << "User Pipe index" << endl;
        cerr << "\tin: " << input_user_pipe_idx << endl
             << "\tout:" << output_user_pipe_idx << endl;
        #endif

        // cerr << "Start Fork" << endl;
        do {
            pid = fork();
            usleep(5000);
        } while (pid < 0);

        if (pid > 0) {
            /* Parent Process */
            #if 0
                cerr << "Parent PID: " << getpid() << endl;
                cerr << "\tNumber of Pipes: " << pipes.size() << endl;
                cerr << "\tNumber of N Pipes: " << number_pipes.size() << endl;
            #endif
            /* Close Pipe */
            // Normal Pipe
            if (i != 0) {
                // cerr << "Parent Close pipe: " << i-1 << endl;
                close(context->pipes[i-1].in);
                close(context->pipes[i-1].out);
            }

            // Number Pipe
            for (int x=0; x < context->number_pipes.size(); ++x) {
                if (context->number_pipes[x].number == 0) {
                    #if 0
                    cerr << "Parent Close number pipe: " << x << endl;
                    #endif
                    close(context->number_pipes[x].in);
                    close(context->number_pipes[x].out);

                    // Remove number pipe
                    context->number_pipes.erase(context->number_pipes.begin() + x);
                    --x;
                }
            }

            // User Pipe
            // if (input_user_pipe_idx != -1) {
            //     close(user_pipes[input_user_pipe_idx].pipe.in);
            //     close(user_pipes[input_user_pipe_idx].pipe.out);
            //     user_pipes[input_user_pipe_idx].is_done = true;
            //     clean_user_pipe();
            // }

            if (is_final_cmd && !(is_number_pipe || is_error_pipe) && !is_output_user_pipe) {
                // Final process, wait
                #if 0
                cerr << "Parent Wait Start" << endl;
                #endif
                int st;
                waitpid(pid, &st, 0);
                #if 0
                cerr << "Parent Wait End: " << st << endl;
                #endif
            }
        } else {
            /* Child Process */
            #if 0
            usleep(2000);
            cerr << "Child PID: " << getpid() << endl;
            cerr << "\tFirst? " << (is_first_cmd ? "True" : "False") << endl;
            cerr << "\tFinal? " << (is_final_cmd ? "True" : "False") << endl;
            cerr << "\tNumber? " << (is_number_pipe ? "True" : "False") << endl;
            cerr << "\tError? " << (is_error_pipe ? "True" : "False") << endl;
            #endif
            #if 0
            cerr << "Child Execute: " << args[0] << endl;
            usleep(5000);
            #endif

            /* Duplicate pipe */
            // STDERR -> socket
            dup2(user_shm_ptr[uid-1].sockfd, STDERR_FILENO);

            if (is_first_cmd) {
                // Receive input from number pipe
                for (size_t x = 0; x < context->number_pipes.size(); x++) {
                    if (context->number_pipes[x].number == 0) {
                        dup2(context->number_pipes[x].in, STDIN_FILENO);
                        #if 0
                        cerr << "First Number Pipe (in) " << context->number_pipes[x].in << " to stdin" << endl;
                        #endif
                        break;
                    }
                }

                // Setup output of normal pipe
                if (context->pipes.size() > 0) {
                    dup2(context->pipes[i].out, STDOUT_FILENO);
                    #if 0
                    cerr << "First Normal Pipe (out) " << context->pipes[i].out << " to stdout" << endl;
                    #endif
                }

                // Recv from user pipe
                if (is_input_user_pipe) {
                    if (is_input_user_pipe_error) {
                        int dev_null = open("/dev/null", O_RDWR);
                        dup2(dev_null, STDIN_FILENO);
                        close(dev_null);
                        #if 0
                        cerr << "Set up user pipe input to null" << endl;
                        #endif
                    } else {
                        // dup2(user_pipes[input_user_pipe_idx].pipe.in, STDIN_FILENO);
                        #if 0
                        cerr << "Set up user pipe input to " << user_pipes[input_user_pipe_idx].pipe.in << endl;
                        #endif
                    }

                }
            }

            // Setup input and output of normal pipe
            if (!is_first_cmd && !is_final_cmd) {
                if (context->pipes.size() > 0) {
                    dup2(context->pipes[i-1].in, STDIN_FILENO);
                    dup2(context->pipes[i].out, STDOUT_FILENO);
                }
                #if 0
                cerr << "Internal (in) " << context->pipes[i-1].in << " to stdin"  << endl;
                cerr << "Internal (out) " << context->pipes[i].out << " to stdout" << endl;
                #endif
                // TODO: user pipe in the middle ??
            }

            if (is_final_cmd) {
                if (is_number_pipe) {
                    /* Number Pipe */
                    #if 0
                    cerr << "Final Number Pipe" << endl;
                    #endif
                    
                    // Setup Input
                    if (context->pipes.size() > 0) {
                        // Receive from previous command via normal pipe
                        dup2(context->pipes[i-1].in, STDIN_FILENO);
                        #if 0
                        cerr << "Final number Pipe (in) (from normal pip) " << context->pipes[i-1].in << " to stdin" << endl;
                        #endif
                    }
                    
                    // Setup Output
                    for (size_t x = 0; x < context->number_pipes.size(); x++) {
                        if (context->number_pipes[x].number == command.number) {
                            dup2(context->number_pipes[x].out, STDOUT_FILENO);
                            close(context->number_pipes[x].out);
                            #if 0
                            cerr << "Final Number Pipe (out) " << context->number_pipes[x].out << " to stdout" << endl;
                            #endif
                            break;
                        }
                    }
                } else if (is_error_pipe) {
                    #if 0
                    cerr << "Final Error Pipe" << endl;
                    #endif
                    /* Error Pipe */
                    for (size_t x = 0; x < context->number_pipes.size(); x++) {
                        if (context->number_pipes[x].number == command.number) {
                            dup2(context->number_pipes[x].out, STDOUT_FILENO);
                            dup2(context->number_pipes[x].out, STDERR_FILENO);
                            break;
                        }
                    }
                } else if (is_output_user_pipe) {
                    #if 0
                    cerr << "Final User Pipe" << endl;
                    #endif

                    if (context->pipes.size() > 0) {
                        // Set input
                        dup2(context->pipes[i-1].in, STDIN_FILENO);
                    }

                    // Set output
                    if (is_output_user_pipe_error) {
                        int dev_null = open("/dev/null", O_RDWR);
                        dup2(dev_null, STDOUT_FILENO);
                        close(dev_null);
                    } else {
                        // dup2(user_pipes[output_user_pipe_idx].pipe.out, STDOUT_FILENO);
                    }

                } else {
                    #if 0
                    cerr << "Final Normal Pipe" << endl;
                    #endif
                    /* Normal Pipe*/
                    if (context->pipes.size() > 0) {
                        dup2(context->pipes[i-1].in, STDIN_FILENO);
                        #if 0
                        cerr << "Set input from " << context->pipes[i-1].in << " to stdin" << endl;
                        #endif
                    }

                    // Redirect to socket
                    dup2(user_shm_ptr[uid-1].sockfd, STDOUT_FILENO);
                    #if 0
                    cerr << "Set output to socket " << user_shm_ptr[uid-1].sockfd << endl;
                    #endif
                }
            }

            /* Close pipe */
            for (int ci = 0; ci < context->pipes.size(); ci++) {
                close(context->pipes[ci].in);
                close(context->pipes[ci].out);
            }
            for (int ci = 0; ci < context->number_pipes.size(); ci++) {
                close(context->number_pipes[ci].in);
                close(context->number_pipes[ci].out);
            }
            // for (int x=0; x < user_pipes.size(); ++x) {
            //     close(user_pipes[x].pipe.in);
            //     close(user_pipes[x].pipe.out);
            // }

            execute_command(uid, args);
        }
    }
    context->pipes.clear();
    return 0;
}

int handle_command(int uid, string input, Context *context) {
    vector<Command> lines;
    int code;

    lines = parse_number_pipe(input);

    for (size_t i = 0; i < lines.size(); i++) {
        code = main_executor(uid, lines[i], context);
    }

    return code;
}

int run_shell(int uid, string input, Context *context) {
    if (input.size() == 0) {
        return 0;
    }

    // Hanld input command
    return handle_command(uid, input, context);
}

void serve_client(int uid) {
    welcome(uid);
    login_prompt(uid);
    command_prompt(uid);
    Context context;

    // Set default PATH
    context.env["PATH"] = "bin:.";
    setenv("PATH", "bin:.", 1);

    while (true) {
        string input = read_msg(user_shm_ptr[uid-1].sockfd);
        context.original_input = input;

        // Run shell
        run_shell(uid, input, &context);
        command_prompt(uid);
    }
}

int main(int argc,char const *argv[]) {
    if (argc != 2) {
        cout << "Usage: prog port" << endl;
        exit(0);
    }

    /* Initialize shared memory */
    init_shm();
    init_lock();

    /* Setup server signal handler */
    signal(SIGCHLD, signal_server_handler);
    signal(SIGINT, signal_server_handler);
    signal(SIGQUIT, signal_server_handler);
    signal(SIGTERM, signal_server_handler);

    /* Variables */
    struct sockaddr_in c_addr;
    int client_sock, c_addr_len;
    int status_code;

    // Initilize variables
    bzero((char *)&c_addr, sizeof(c_addr));
    c_addr_len = sizeof(c_addr);
    listen_sock = get_listen_socket(argv[1]);

    while (true) {
        client_sock = accept(listen_sock, (struct sockaddr *) &c_addr, (socklen_t *) &c_addr_len);
        if (client_sock < 0) {
            perror("Sever accept");
            exit(0);
        }

        if (is_user_up_to_limit()) {
            cerr << "Online users are up to limit (" << USER_LIMIT << ")" << endl;
            close(client_sock);
            continue;
        } 
        pid_t pid = fork();

        if (pid > 0) {
            /* Parent */
            close(client_sock);
        } else if (pid == 0) {
            /* Child */
            // Setup signal handler
            signal(SIGUSR1, signal_child_handler);
            signal(SIGUSR2, signal_child_handler);
            signal(SIGINT, signal_child_handler);
            signal(SIGQUIT, signal_child_handler);
            signal(SIGTERM, signal_child_handler);
            // Create user
            int uid = create_user(client_sock, c_addr);
            serve_client(uid);
        } else {
            perror("Server fork");
            server_exit();
        }
        
        #if 1
        debug_user();
        #endif
    }
}