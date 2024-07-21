#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <wait.h>
#include <string.h>
#include <unistd.h>
#include <netinet.h>
#include <fcntl.h>
#include <types.h>
#include <epoll.h>
#include <stat.h>

#define MAXSIZE 2048

void sys_err(const char* str) {
	perror(str);
	exit(1);
}

//获取一行 \r\n结尾的数据
//HTTP协议头部的每一个字段结束都是通过\r\n来表示的
//例如：GET /index.html HTTP/1.1\r\n
//      Host: www.example.com\r\n
//      User - Agent: curl / 7.64.0\r\n
//      Accept : */*\r\n
//      \r\n
int get_line(int cfd, char* buf, int size) {

	int i = 0;
	char c = '\0';
	int n;

	//读取一行数据
	while ((i < size - 1) && (c != '\n')) {

		//recv函数:从连接的套接字接收数据
		//参数：已连接的套接字、读入的缓冲区、缓冲区大小、标志位（一般设为0）
		//每次读取单个字符
		//当标志位为0时，代表从缓冲区取走数据，
		//当标志位为MSG_PEEK时，代表只是查看数据，而不取走数据
		//>0：读到的数据长度大小
		//=0：连接关闭
		//<0：出错
		n = recv(cfd, &c, 1, 0);

		//成功读取
		if (n > 0) {
			//读取到回车符 \r
			if (c == '\r') {
				//查看数据，即读取下一个数据
				n = recv(cfd, &c, 1, MSG_PEEK);
				//读取到换行符 \n
				if ((n > 0) && (c == '\n')) {
					//将\n 读到缓冲区中
					recv(cfd, &c, 1, 0);
				}
				else {
					c = '\n';
				}
			}
			buf[i] = c;
			i++;
		}
		else {
			c = '\n';
		}
	}
	//读取完毕，添加字符串结束标志到buf尾
	buf[i] = '\0';

	//读取出错（没有读完时，i=0；当协议全部读完，i=-1）
	if (n == -1) {
		i = n; 
	}

	return i;
}

int init_listen_fd(int port, int epfd) {

	struct sockaddr_in serv_addr;

	//将地址结构清零
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr = htonl(INADDR_ANY);

	int lfd;
	//创建一个套接字
	//成功：套接字所对应的文件描述符；失败：-1 
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		sys_err("socket error");
	}

	//设置端口复用
	int opt = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	int ret;
	//绑定地址结构（IP+端口），成功是0，失败-1
	ret = bind(lfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	if (ret == -1) {
		sys_err("bind error");
	}

	//设置同时与服务器进行连接的客户端上限数，成功是0，失败-1
	ret = listen(lfd, 128);
	if (ret == -1) {
		sys_err("listen error");
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = lfd;

	//添加fd到epoll树上，成功0，失败-1 
	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1) {
		sys_err("epoll_ctl error");
	}

	return lfd;
}

void do_accept(int lfd, int epfd) {

	struct sockaddr_in clit_addr;
	socklen_t clit_addr_len = sizeof(clit_addr);

	//服务器与客户端连接
	int cfd;
	cfd = accept(lfd, (struct sockaddr*)&clit_addr, &clit_addr_len);
	if (cfd == -1) {
		sys_err("accept error");
	}

	//打印客户端IP+端口号
	char client_IP[1024] = { 0 };
	printf("New Client IP: %s, Port: %d, cfd = %d\n",inet_ntop(AF_INET, 
		&clit_addr.sin_addr.s_addr, client_IP, 
		sizeof(client_IP)), ntohs(clit_addr.sin_port),cfd);

	//设置cfd为非阻塞
	int flag = fcntl(cfd, F_GETFL);
	flag |= O_NONBLOCK;;
	fcntl(cfd, F_SETFL, flag);

	//将新节点cfd挂到epoll监听树上
	struct epoll_event ev;
	ev.data.fd = cfd;

	//边沿非阻塞模式（ET模式）
	ev.events = EPOLLIN | EPOLLET;

	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1) {
		sys_err("epoll_ctl_add error");
	}
}

//客户端关闭，移除监听树上的监听事件
void disconnect(int cfd, int epfd) {

	int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, CFD, NULL);
	if (ret == -1) {
		sys_err("epoll_ctl_del error");
	}
	close(cfd);
}

//参数：
//cfd：客户端的fd；no：状态码；disp：状态描述
//type：回发文件类型；len：文件长度
//200：服务器成功返回网页;404：请求的网页不存在；503：服务器超时
void send_respond(int cfd, int no, char* disp, char* type, int len) {

	char buf[4096] = { 0 };

	sprintf(buf, "HTTP/1.1 %d %s\r\n", no, disp);
	send(cfd, buf, strlen(buf), 0);
	memset(buf, 0, sizeof(buf));

	sprintf(buf, "Content-Type:%s\r\n", type);
	sprintf(buf + strlen(buf), "Content-Length:%d\r\n", len);
	send(cfd, buf, strlen(buf), 0);

	send(cfd, "\r\n", 2, 0);
}

void send_file(int cfd, const char* file) {

	int n = 0;
	int ret;
	char buf[4096] = { 0 };

	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		sys_err("open error");
	}

	while ((n == read(fd, buf, sizeof(buf))) > 0) {
		ret = send(cfd, buf, n, 0);
		if (ret == -1) {
			printf("errno = %d\n", errno);

			if (errno == EAGAIN) {
				printf("---------------EAGAIN\n");
				continue;
			}
			else if (errno == EINTR) {
				printf("--------------- EINTR\n");
				continue;
			}
			else {
				sys_err("send error");
			}
		}

		if (ret < 4096) {
			printf("----------send ret:%d\n", ret);
		}
	}
	close(fd);
}

//处理http请求，判断文件是否存在，然后回发
void http_request(int cfd, const char* file) {

	struct stat sbuf;

	//判断文件是否存在
	//stat函数：获取文件(file指针指向的文件名）信息
	int ret = stat(file, &sbuf);
	if (ret == -1) {
		sys_err("stat error");
	}

	//是一个普通类型的文件
	if (S_ISREG(sbuf.st_mode)) {

		//回发http应答协议
		send_respond(cfd, 200, "OK", "text/plain; charset=iso-8859-1",
			sbuf.st_size);

		//send_respond(cfd, 200, "OK", "image/jepg", -1); //图片
		//send_respond(cfd, 200, "OK", "audio/mepg", -1); //音频

		//回发给客户端请求数据内容
		send_file(cfd, file);
	}
}

void do_read(int cfd, int epfd) {

	//读取一行http协议，拆分，获取 GET 文件名 协议号
	char line[1024] = { 0 };
	char method[16], path[256], protocol[16];

	//读http请求协议首行 GET /test.c HTTP/1.1
	int len = get_line(cfd, line, sizeof(line));
	if (len == 0) {
		printf("服务器检查到客户端关闭...\n");
		disconnect(cfd, epfd);
	}
	else {
		//解析读取的数据
		sscanf(line, "%[^ ] %[^ ] %[^ ]", method, path, protocol);
		printf("method = %s, path = %s, protocol = %s\n",
			method, path, protocol);

		while (1) {
			char buf[1024] = { 0 };
			len = get_line(cfd, buf, sizeof(buf));
			//读取一行，碰到 \r\n 就停止
			if (buf[0] == '\n') { 
				break;
			}
			//协议全部读完
			else if (len == -1) {
				break;
			}
		}
	}

	//strncasecmp()用来比较参数s1 和s2 字符串前n个字符，比较时会自动忽略大小写的差异。
	//若参数s1 和s2 字符串相同则返回0。
	//s1 若大于s2 则返回大于0 的值。
	//s1 若小于s2 则返回小于0 的值。
	if (strncasecmp(method, "GET", 3) == 0) {
		//file来指向文件名
		// path = ‘/’ ，文件名从（path + 1）开始
		char* file = path + 1;

		http_request(cfd, file);

		disconnect(cfd, epfd);
	}
}

void epoll_run(int port) {

	//循环因子
	int i;
	//监听的事件
	struct epoll_event all_events[MAXSIZE];

	//建立一个监听红黑树
	//返回值：成功 指向创建的红黑树的根节点的fd；失败 -1
	int epfd = epoll_create(MAXSIZE);
	if (epfd == -1) {
		sys_err("epoll_create error");
	}

	//创建lfd，并添加到监听树上
	int lfd = init_listen_fd(port, epfd);

	//epoll_wait 监听
	while (1) {
		int ret = epoll_wait(epfd, all_events, MAXSIZE, -1);
		if (ret == -1) {
			sys_err("epoll_wait error");
		}

		//遍历all_events 中的事件
		for (int i = 0; i < ret; i++) {

			//接收满足事件的fd
			int sfd = all_events[i].data.fd;

			//不是读事件
			if (!(sfd.events & EPOLLIN)) {
				continue;
			}

			//如果sfd=lfd，就说明有客户端要来连接
			if (sfd == lfd) {
				do_accept(lfd, epfd);
			}

			//如果不是lfd，就说明有读事件发生
			else {
				do_read(sfd, epfd);
			}
		}
	}
}

int main(int argc, char *argv) {

	if (argc < 3) {
		printf("Please input 2 parameters!\n");
	}

	//获取从命令行输入的端口号
	int port = atoi(argv[1]);

	//chdir函数，用于客户端打开服务器的目录
	//改变进程的工作目录，成功返回0，失败返回-1
	//在Centos7上默认是/home，可以自行改变文件所属位置
	int ret = chdir(argv[2]);
	if (ret != 0) {
		sys_err("chdir error");
	}

	//epoll监听
	epoll_run(port);

	return 0;

}
