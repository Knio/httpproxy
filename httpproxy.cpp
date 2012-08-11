/*

Tom Flanagan

To Compile: g++ a1.cpp -Wall -o proxy

To Run: ./proxy -p PORT -v DEBUG

The server will listen on port PORT (or 1234 if not specified).

DEBUG=0 only error messages will be printed
DEBUG=1 connection messages and URLs retrieved will be printed (default)
DEBUG=2 all data going through the proxy will be printed
DEBUG=3 even more debug info

This proxy supports multiple concurrent *HTTP/1.1* client connections
with keep-alive and pipelining. This proxy will block banned words
in the URL and content with the appropriate 302 redirect method.
If the client requests an error page with an "If-Modified-Since" header,
the proxy will automatically respond with 304 Not Modified without asking
the server.

*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <vector>
#include <map>

#ifdef WIN32
    #include <winsock.h>
    #typedef socklen_t int
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

#define PORT 1234

#define SIZE (102400) // 100kb buffer

using namespace std;

char buffer[SIZE];
int debug = 1;

char* BANNED[]  = { "Sponge Bob",
                    "SpongeBob",
                    "Barry Manilow",
                    "Edmonton Oilers",
                    "BarryManilow",
                    "EdmontonOilers"
                    "Sponge%20Bob",
                    "Barry%20Manilow",
                    "Edmonton%20Oilers"
                  };

char* BADURL        = "Sorry, but the Web page that you were trying\
 to access is inappropriate for you, based on the URL. The page has\
 been blocked to avoid insulting your intelligence. \r\nNet Ninny";

char* BADCONTENT    = "Sorry, but the Web page that you were trying\
 to access is inappropriate for you, based on some of the words it \
 contains. The page has been blocked to avoid insulting your intell\
 igence. \r\nNet Ninny";

char* ERROR1URL = "http://pages.cpsc.ucalgary.ca/~carey/CPSC441/ass1/error1.html";
char* ERROR2URL = "http://pages.cpsc.ucalgary.ca/~carey/CPSC441/ass1/error2.html";



// fatal error. prints an error message and quits the program
void error(char* msg)
{
    printf("\nFatal Error: %s\n", msg);
    exit(1);
}


// helper to convert things to std::strings
template <typename T> string tostring(T x) {
    stringstream ss;
    ss << x;
    return ss.str();
}

int strfind(string data, string needle)
{
    if (data.length()<needle.length()) return 0;
    for (size_t i=0;i<data.length()-needle.length();i++)
    {
        size_t k;
        for (k=0;k<needle.length();k++)
        {
            if ((data[i+k]|32) != (needle[k]|32)) break;
        }
        if (k==needle.length()) return 1;
    }
    return 0;
}

string tolower(string data)
{
    string dat2;
    for (size_t i=0;i<data.length();i++)
    {
        dat2 += data[i]|32;
    }
    return dat2;
}


// parses urls. "protocol://host/path"
class urltype
{
  public:
    int type;
    string protocol;
    string host;
    string path;

    urltype(){}
    urltype(string url) { parse(url); }

    // parses a url
    void parse(string url)
    {
        size_t i = url.find("://");
        if (i!=string::npos)
        {
            type = 1;
            protocol = url.substr(0,i);
            url.erase(0, i+3);
        }
        else
        {
            type = 2;
            protocol = "http";
        }

        i = url.find("/");
        if (i==string::npos)
        {
            host = url;
            path = "";
        }
        else
        {
            host = url.substr(0, i);
            path = url.substr(i);
        }
    }

    // returns the string representation of this url
    string render()
    {
        if (type==1) return protocol + "://" + host + path;
        else         return path;
    }


};


// generic class representing an HTTP message
class httpmessage
{
  private:
    string buff;

  public:
    virtual ~httpmessage() {}

    int type;
    int status;
    string http;
    map<string,string> header;
    string data;

    httpmessage()
    {
        type    = 0;
        status  = 0;
        http    = "HTTP/1.1";
    }

    int read(char* data, int n)
    {
        string d;
        d.append(data, n);
        return read(d);
    }

    // parses some data into the message. returns the number of bytes that were used
    int read(string data)
    {
        if (debug>= 3) printf("D: status=%d read: %s\nD.\n", status, data.c_str());
        buff += data;

        size_t i;
        while ((i=buff.find("\r\n")) != string::npos && (status == 0 || status == 1))
        {
            readline(buff.substr(0, i));
            buff = buff.substr(i+2);
        }

        if (status == 2)
        { // How do we know when we are done getting data?
            string cl("Content-Length");
            this->data += buff;
            buff.erase();

            if (header.count(cl))
            { // 1. if there is a C-L header, do what it says.
                size_t clv = (size_t)atoi(header[cl].c_str());
                if (clv == this->data.length())
                {
                    status = 3;
                }
                if (clv < this->data.length())
                {
                    status = 3;
                    buff = this->data.substr(clv); // return unused bytes
                    this->data = this->data.substr(0,clv);
                    return data.length()-buff.length();
                }
            }
            else
            { // No header. what to do?!?
                if (type==1)
                { // this is a request. assume that there is no content.
                    status = 3;
                    buff = this->data;
                    this->data.erase();
                    return data.length()-buff.length(); // hope this is positive..
                }
                if (type==2)
                { // this is a response. is there a Connection header?
                    string cn("Connection");
                                             // google sends capitalized headers :(
                    if (!header.count(cn) || (tolower(header[cn]) != "close"))
                    { // there is no connection header, or it's not going to close.
                      // assume we have everything.
                        status = 3;
                        return data.length();
                    }
                    // else, the connection will close when we are done.
                    // let close() handle it.
                }
            }
        }

        return data.length();
    }

    // peer closed the connection. determine if we are done, or there was an error
    void close()
    {
        if (status == 3)
            return;

        if (status != 2)
        {
            status = -1;
            return;
        }

        if (!header.count("Content-Length"))
        {
            status = 3;
            return;
        }
        printf("error: peer prematurely closed connection: Content-Length: %s, data=%d\n",
            header["Content-Length"].c_str(), data.length());
        status = -1;
    }

    // parses a single line
    void readline(string line)
    {
        switch (status)
        {
            case 0: readfirst(line);
                    break;

            case 1: readheader(line);
                    break;
        }
    }

    // read the first line. this is different for requests and responses
    virtual void readfirst(string) = 0;

    // read a header line
    void readheader(string line)
    {
        if (line == "")
        {
            status = 2;
            return;
        }
        size_t i = line.find(": ");
        if (i == string::npos)
        {
            i = line.find(":"); // rad.msn.com forgets the space in one of their headers :(
            if (i == string::npos)
            {
                printf("error: invalid header '%s'\n", line.c_str());
                status = -1;
                return;
            }
            header[line.substr(0,i)] = line.substr(i+1);
        }
        else
            header[line.substr(0,i)] = line.substr(i+2);

    }


    virtual void print()
    {
        printf("status=%d\n", status);
        for (map<string,string>::iterator i=header.begin();i!=header.end();i++)
            printf("header: %s: %s\n", i->first.c_str(), i->second.c_str());
        printf("data=%s\n", data.c_str());
    }

    // return the HTTP message in its natural form
    virtual string render()
    {
        string r;
        r += renderfirst() + "\r\n";
        for (map<string,string>::iterator i=header.begin();i!=header.end();i++)
            r += i->first + ": " + i->second + "\r\n";
        r += "\r\n";
        r += data;

        return r;
    }

    virtual string renderfirst() = 0;

};

// HTTP request object
class httprequest: public httpmessage
{
  public:
    virtual ~httprequest() {}

    string  method;
    urltype url;

    httprequest() : httpmessage()
    {
        type   = 1;
        status = 0;
    }

    // parse the first line. "METHOD URL PROTOCOL", ie "GET /index.html HTTP/1.1"
    void readfirst(string line)
    {
        size_t i = line.find(" ");
        if (i == string::npos)
        {
            printf("error: no method\n");
            status = -1;
            return;
        }
        method = line.substr(0, i);
        line.erase(0, i+1);

        i = line.find(" ");
        if (i == string::npos)
        {
            printf("error: no url\n");
            status = -1;
            return;
        }
        url.parse(line.substr(0, i));
        http = line.substr(i+1);

        if (http != "HTTP/1.0" && http != "HTTP/1.1")
        {
            printf("error: invalid http version\n");
            status = -1;
            return;
        }

        status = 1;
    }


    void print()
    {
        printf("httprequest\n");
        printf("method=%s, url=%s, http=%s\n", method.c_str(), url.render().c_str(), http.c_str());
        httpmessage::print();
    }

    string renderfirst()
    {
        string r;
        r += method + " " + url.render() + " " + http;
        return r;
    }

};

// HTTP response object
class httpresponse: public httpmessage
{

  public:
    virtual ~httpresponse(){}
    string code;
    string reason;

    httpresponse():httpmessage()
    {
        type    = 2;
        code    = "200";
        reason  = "OK";
    }

    // parse the first line. ie: "HTTP/1.1 200 OK"
    void readfirst(string line)
    {
        size_t i = line.find(" ");
        if (i == string::npos)
        {
            printf("error: no http\n");
            status = -1;
            return;
        }
        http = line.substr(0, i);
        line.erase(0, i+1);

        i = line.find(" ");
        if (i == string::npos)
        {
            printf("error: no response code\n");
            status = -1;
            return;
        }
        code = line.substr(0, i);
        reason = line.substr(i+1);

        if (http != "HTTP/1.0" && http != "HTTP/1.1")
        {
            printf("error: invalid http: %s\n", http.c_str());
            status = -1;
            return;
        }

        status = 1;
    }

    string renderfirst()
    {
        string r;
        r += http + " " + code + " " + reason;
        return r;
    }

};



// handles a single client connection to the proxy
class proxyhandler
{

  public:

    int         sock;
    sockaddr_in addr;


    proxyhandler(int s, sockaddr_in a)
    {
        sock = s;
        addr = a;
    }

    ~proxyhandler()
    {
        close(sock);
    }

    // keep processing requests until the client disconnects
    void main()
    {
        int requests = 0;
        if (debug) printf("Processing connection from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        //process();
        while (process() != -1) requests++;
        if (debug) printf("Finished connection from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        if (debug) printf("Served %d requests to the client\n", requests);
    }

    // read a single request from the client, and serve the appropriate response
    int process()
    {
        httprequest     req;
        httpresponse    res;

        if (recv(sock, req) == -1)
        {  // client disconnected
            return -1;
        }
        if (debug>=2) printf("CLIENT->PROXY:\n\n%s\n\n", req.render().c_str());

        if (debug>=1) printf("  CLIENT: %s\n", req.url.render().c_str());
        int r = load(req,  res);

        res.header["Proxy-Connection"] = "close";
        res.header["Connection"] = "close";

        if (r != -1 && req.header["Connection"] == "keep-alive" || req.header["Proxy-Connection"] == "keep-alive")
        {
            res.header["Proxy-Connection"] = "keep-alive";
            res.header["Connection"] = "keep-alive";
        }

        if (debug>=2) printf("PROXY->CLIENT:\n\n%s\n\n", res.render().c_str());
        send(sock, res);

        return r;
    }

    int load(httprequest &req, httpresponse &res)
    {
        if (req.status != 3)
        {
            res = response(400, "Bad Request", "Error while parsing your browsers request");
            res.header["Connection"] = "close";
            return -1; // close the client connection, because buffers might be messed up
        }

        if (req.url.protocol != "http") // tried to proxy https://, ftp://, etc
        {
            res = response(400, "Bad Request", "Only HTTP protocol is supported");
            return 0;
        }

        if (banned(req.url.render())) // bad URL
        {
            res = response(302, "Page Moved", BADURL);
            res.header["Location"] = ERROR1URL;
            return 0;
        }

        if (req.url.render() == ERROR1URL || req.url.render() == ERROR2URL)
        {
            if (req.header.count("If-Modified-Since"))
            {
                res = response(304, "Not Modified", "");
                return 0;
            }
        }

        hostent* he = gethostbyname(req.url.host.c_str());
        if (he == NULL)
        {
            res = response(404, "Bad Request", tostring("Host ")+req.url.host+" was not found");
            return 0;
        }
        sockaddr_in servaddr;

        servaddr.sin_family = AF_INET;
        servaddr.sin_port   = htons(80);
        servaddr.sin_addr = *((struct in_addr*)he->h_addr);
        memset(servaddr.sin_zero, '\0', sizeof servaddr.sin_zero);

        int serv = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(serv, (struct sockaddr *)&servaddr, sizeof servaddr) == -1)
        {
            res = response(504, "Could Not Connect", tostring("Could not connect to remote server ") + req.url.host);
            return 0;
        }

        map<string,string> temp = req.header;
        req.header["Connection"] = "close"; // HTTP/1.1 doesn't quite work on the client side
        req.url.type = 2;

        if (debug>=2) printf("PROXY->SERV:\n\n%s\n\n", req.render().c_str());
        if (send(serv, req) == -1)
        {
            res = response(502, "Server Error", "Error sending request to remote server");
            return 0;
        }
        if (recv(serv, res) == -1)
        {
            //printf("SERV->PROXY (recv error):\n\n%s\n\n", res.render().c_str());
            res = response(502, "Server Error", "Error reading response from remote server");
            return 0;
        }

        if (debug>=2) printf("SERV->PROXY (good) (%d bytes):\n\n%s\n\n", res.data.length(), res.render().c_str());
        close(serv);

        req.header = temp;

        if (banned(res.data)) // bad content
        {
            res = response(302, "Page Moved", BADCONTENT);
            res.header["Location"] = ERROR2URL;
            return 0;
        }

        if (!res.header.count("Content-Length")) // the server might not have sent a C-L header, but we need one so the client can read >1 responses.
            res.header["Content-Length"] = tostring(res.data.length());

        else if (atoi(res.header["Content-Length"].c_str()) != (int)res.data.length())
                error("content-length mismatch"); // this should never happen, httpmessage checks it


        return 0;
    }

    // create a custom message
    httpresponse response(int code, string reason, string data)
    {
        httpresponse res;
        res.http    = "HTTP/1.1";
        res.code    = tostring(code);
        res.reason  = reason;
        res.data    = data;
        res.header["Content-Length"] = tostring(res.data.length());
        res.header["Content-Type"] = "text/plain";
        return res;
    }

    // recv an httpmessage from a socket. returns -1 on failure, bytes read on success
    int recv(int sock, httpmessage &msg)
    {
        int bytes = 0;
        while (msg.status != 3 && msg.status != -1)
        {   // peek - we might not want all of the bytes here, as some may be for a future pipelined message, and not this message.
            int r = ::recv(sock, buffer, SIZE, MSG_PEEK);
            if (debug>= 3) printf("got %d bytes\n", r);
            if (r == -1)
            {
                error("recv");
                return -1;
            }

            if (r==0)
            { // peer closed the connection
                msg.close();
                if (debug>= 3) printf("peer closed connection. status=%d\n", msg.status);
                if (msg.status == -1)   return -1;
                else                    return bytes;
            }

            int rr = msg.read(buffer, r);
            if (debug>= 3) printf("httpmessage (status=%d) wanted %d bytes\n", msg.status, rr);
            if (rr != ::recv(sock, buffer, rr, 0))
            {   // clear the buffer of bytes we acctually wanted
                error("error reading socket");
            }
            bytes += rr;
        }
        return msg.status==-1 ? -1 : bytes;
    }

    // send an httpmessage to a socket. returns -1 on failure, number of bytes send on success
    int send(int sock, httpmessage &msg)
    {
        string buff = msg.render();
        int i       = 0;
        int j       = buff.length();

        while (i != j && i != -1)
        {
            i += ::send(sock, buff.c_str()+i, j-i, 0);
        }
        return i;
    }

    int banned(string data)
    {
        for (unsigned int i=0;i<sizeof(BANNED)/sizeof(char*);i++)
        {
            if (strfind(data, BANNED[i]))
            {
                if (debug) printf("FOUND: %s\n", BANNED[i]);
                return 1;
            }
        }
        return 0;
    }

};


// proxy main. listen for and accept new requests, forking off to a proxyhandler object.
int main(int argv, char**argc)
{
    int port = PORT;

    vector<string> args;
    for (int i=1;i<argv;i++)
        args.push_back(argc[i]);

    while (args.size() >= 2)
    {
        if (args[0] == "-p")
        {
            port = atoi(args[1].c_str());
            args.erase(args.begin(),args.begin()+2);
        }
        else if (args[0] == "-v")
        {
            debug = atoi(args[1].c_str());
            args.erase(args.begin(),args.begin()+2);
        }
        else
        {
            printf("'%s' invalid parameter\n", args[0].c_str());
            args.erase(args.begin());
        }

    }

    printf("HTTP Proxy listening on port %d\n", port);

    int                 listener;
    struct sockaddr_in  addr;

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    memset(addr.sin_zero, '\0', sizeof addr.sin_zero);

    listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener == -1)
        error("create socket");

    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == -1)
        error("bind");

    if (listen(listener, 10) == -1)
        error("listen");

    while (1)
    {
        socklen_t size;
        struct sockaddr_in newaddr;

        int newsock = accept(listener, (struct sockaddr *)&newaddr, &size);
        if (newsock == -1)
            error("accept");

        if (debug) printf("connection from %s\n", inet_ntoa(newaddr.sin_addr));

        if (!fork())
        {
            close(listener);
            proxyhandler px(newsock, newaddr);
            px.main();
            return 0;
        }
        close(newsock);
    }

    return 0;
}
