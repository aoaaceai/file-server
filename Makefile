all: server.cpp client.cpp
	g++ server.cpp -o server
	g++ client.cpp -o client
clean:
	rm -rf server client
	rm -rf server_dir client_dir
	mkdir server_dir client_dir 2> /dev/null || true
	echo abcdefg > server_dir/a
	touch server_dir/b server_dir/c
	echo abc > client_dir/file
