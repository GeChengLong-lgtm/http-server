# http-server
基于Linux的轻量级多线程高并发服务器

项目流程：
1. getline() 获取 http协议的第一行。

2. 从首行中拆分  GET、文件名、协议版本。 获取用户请求的文件名。

3. 判断文件是否存在--stat()

4. 判断是文件还是目录。

5. 是文件-- open -- read -- 写回给浏览器

6. 先写 http 应答协议头 ： 	http/1.1 200 ok
			
Content-Type：text/plain; charset=iso-8859-1 

7. 写文件数据。
