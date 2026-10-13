#include "entrypoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <iostream>

#include <winsock2.h>
#include <ws2tcpip.h>

#define DEFAULT_BUFLEN 512     // taille du buffer pour recv

SOCKET ConnectSocket = INVALID_SOCKET;
char recvbuf[DEFAULT_BUFLEN];

// se connecte au serveur addr:port (ex: "example.com", "80")
// le socket connecte est mis dans ConnectSocket
// retourne 1 si ok, -1 sinon
int connect_client(const char* addr, const char* port)
{
    struct addrinfo *result = NULL, *ptr = NULL, hints;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // trouve l'adresse IP du serveur (ex: example.com -> 23.x.x.x)
    int iResult = getaddrinfo(addr, port, &hints, &result);
    if (iResult != 0) {
        printf("getaddrinfo failed: %d\n", iResult);
        return -1;
    }

    // on essaie les adresses une par une jusqu'a ce qu'une marche
    // (ex: localhost peut donner ::1 en IPv6 puis 127.0.0.1 en IPv4)
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        ConnectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (ConnectSocket == INVALID_SOCKET)
            continue;

        if (connect(ConnectSocket, ptr->ai_addr, (int)ptr->ai_addrlen) != SOCKET_ERROR)
            break;

        closesocket(ConnectSocket);
        ConnectSocket = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (ConnectSocket == INVALID_SOCKET) {
        printf("Impossible de se connecter a %s:%s\n", addr, port);
        return -1;
    }
    return 1;
}

// pose une question a l'utilisateur et retourne sa reponse
// si il appuie juste sur Entree, on prend la valeur par defaut
std::string demander(const char* question, const std::string& defaut = "")
{
    std::string reponse;
    if (defaut.empty())
        printf("%s : ", question);
    else
        printf("%s [%s] : ", question, defaut.c_str());
    fflush(stdout);
    if (!std::getline(std::cin, reponse))
        exit(0);
    if (reponse.empty())
        return defaut;
    return reponse;
}

// forme la requete HTTP/1.1 au complet :
//   GET /chemin HTTP/1.1     <- ligne de requete
//   Host: example.com        <- headers (Host est obligatoire en 1.1)
//   ...
//                            <- ligne vide (\r\n) = fin des headers
//   contenu                  <- corps (seulement pour PUT)
std::string faire_requete(const std::string& methode, const std::string& hote, const std::string& port,
                          const std::string& chemin, const std::string& corps)
{
    std::string req = methode + " " + chemin + " HTTP/1.1\r\n";
    // on met le port dans Host seulement si ce n'est pas le port 80
    if (port == "80")
        req += "Host: " + hote + "\r\n";
    else
        req += "Host: " + hote + ":" + port + "\r\n";
    // pour PUT le serveur doit savoir la taille du corps a lire
    if (methode == "PUT")
        req += "Content-Length: " + std::to_string(corps.size()) + "\r\n";
    // on demande au serveur de fermer apres la reponse,
    // comme ca on sait quand on a tout recu
    req += "Connection: close\r\n";
    req += "\r\n";
    req += corps;
    return req;
}

// envoie la requete au serveur et affiche la reponse recue
void envoyer(const std::string& hote, const std::string& port, const std::string& requete)
{
    if (connect_client(hote.c_str(), port.c_str()) < 0)
        return;

    int iResult = send(ConnectSocket, requete.c_str(), (int)requete.size(), 0);
    if (iResult == SOCKET_ERROR) {
        printf("send failed: %d\n", (int)WSAGetLastError());
        closesocket(ConnectSocket);
        return;
    }

    // on lit jusqu'a ce que le serveur ferme la connexion (Connection: close)
    printf("\n----- Reponse -----\n");
    do {
        iResult = recv(ConnectSocket, recvbuf, DEFAULT_BUFLEN, 0);
        if (iResult > 0)
            fwrite(recvbuf, 1, iResult, stdout);
        else if (iResult < 0)
            printf("recv failed: %d\n", (int)WSAGetLastError());
    } while (iResult > 0);
    printf("\n-------------------\n\n");

    closesocket(ConnectSocket);
    ConnectSocket = INVALID_SOCKET;
}

// point d'entree du client (appele par main.cpp)
// affiche le menu en boucle jusqu'a ce qu'on choisisse Quitter
void raylib_start(void)
{
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed: %d\n", iResult);
        exit(1);
    }

    while (true) {
        printf("=== Client HTTP ===\n");
        printf("1. GET\n");
        printf("2. HEAD\n");
        printf("3. PUT\n");
        printf("4. DELETE\n");
        printf("5. Quitter\n");
        std::string choix = demander("Choix");

        std::string methode;
        if (choix == "1") methode = "GET";
        else if (choix == "2") methode = "HEAD";
        else if (choix == "3") methode = "PUT";
        else if (choix == "4") methode = "DELETE";
        else if (choix == "5") break;
        else {
            printf("Choix invalide\n\n");
            continue;
        }

        std::string hote = demander("Serveur ", "localhost");
        // localhost -> notre serveur sur 8080, sinon port HTTP normal 80
        std::string port_defaut = (hote == "localhost" || hote == "127.0.0.1") ? "8080" : "80";
        std::string port = demander("Port", port_defaut);
        std::string chemin = demander("Chemin", "/");
        if (chemin[0] != '/')
            chemin = "/" + chemin;

        std::string corps;
        if (methode == "PUT")
            corps = demander("Contenu du fichier");

        std::string requete = faire_requete(methode, hote, port, chemin, corps);
        printf("\n----- Requete envoyee -----\n%s\n", requete.c_str());

        envoyer(hote, port, requete);
    }

    WSACleanup();
}
