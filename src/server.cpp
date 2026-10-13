#include "entrypoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <map>

#include <winsock2.h>
#include <ws2tcpip.h>

#define DEFAULT_PORT "8080"   // port ou le serveur ecoute
#define DEFAULT_BUFLEN 512     // taille du buffer pour recv

struct addrinfo *result = NULL, hints;
SOCKET ListenSocket = INVALID_SOCKET;

// les "fichiers" du serveur : chemin -> contenu
// ex: ressources["/notes.txt"] = "bonjour"
// tout est garde en memoire, donc ca s'efface quand on ferme le serveur
std::map<std::string, std::string> ressources;

// Prepare le socket du serveur :
// WSAStartup (windows) -> getaddrinfo -> socket -> bind -> listen
// retourne 1 si tout est ok, -1 sinon

int init_server()
{
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed: %d\n", iResult);
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    int iResult2 = getaddrinfo("127.0.0.1", DEFAULT_PORT, &hints, &result);
    if (iResult2 != 0) {
        printf("getaddrinfo failed: %d\n", iResult2);
        WSACleanup();
        return -1;
    }

    ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ListenSocket == INVALID_SOCKET) {
        printf("Error at socket(): %d\n", (int)WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        return -1;
    }

    iResult2 = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iResult2 == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", (int)WSAGetLastError());
        freeaddrinfo(result);
        closesocket(ListenSocket);
        WSACleanup();
        return -1;
    }
    freeaddrinfo(result);

    // a partir d'ici le serveur attend des connexions
    if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("Listen failed with error: %d\n", (int)WSAGetLastError());
        closesocket(ListenSocket);
        WSACleanup();
        return -1;
    }
    return 1;
}

// met une chaine en minuscule (les noms de headers HTTP ne sont pas
// sensibles a la casse : "Host", "host" et "HOST" c'est pareil)
std::string en_minuscule(std::string s)
{
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = s[i] - 'A' + 'a';
    return s;
}

// cherche la valeur d'un header dans la requete
// ex: get_header(entetes, "Host") -> "localhost:8080"
// retourne "" si le header n'existe pas
std::string get_header(const std::string& entetes, const std::string& nom)
{
    std::string bas = en_minuscule(entetes);
    std::string cle = "\r\n" + en_minuscule(nom) + ":";
    size_t pos = bas.find(cle);
    if (pos == std::string::npos)
        return "";
    pos += cle.size();
    size_t fin = entetes.find("\r\n", pos);
    std::string valeur = entetes.substr(pos, fin - pos);
    // enleve les espaces avant et apres la valeur
    while (!valeur.empty() && valeur[0] == ' ')
        valeur.erase(0, 1);
    while (!valeur.empty() && valeur[valeur.size() - 1] == ' ')
        valeur.erase(valeur.size() - 1);
    return valeur;
}

// date actuelle au format HTTP pour le header Date
// ex: "Tue, 13 Oct 2026 14:16:52 GMT"
std::string date_http()
{
    char buf[64];
    time_t maintenant = time(NULL);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&maintenant));
    return buf;
}

// petite page html utilisee comme corps pour les messages (404, cree, etc.)
std::string page(const std::string& titre)
{
    return "<html><body><h1>" + titre + "</h1> <h2>subscribe to pewdiepie</body></html>\n";
}

// construit la reponse HTTP au complet :
//   HTTP/1.1 200 OK          <- ligne de statut
//   Date: ...                <- headers
//   Content-Length: ...
//                            <- ligne vide (\r\n) = fin des headers
//   <html>...                <- corps
// envoyer_corps = false pour HEAD (on envoie juste les headers)
// autres = headers en plus (ex: Allow pour le 405)
std::string faire_reponse(int code, const std::string& texte,
                          const std::string& corps, bool envoyer_corps,
                          const std::string& autres = "")
{
    std::string rep = "HTTP/1.1 " + std::to_string(code) + " " + texte + "\r\n";
    rep += "Date: " + date_http() + "\r\n";
    rep += autres;
    // une reponse 204 n'a pas de corps, donc pas de Content-Length
    if (code != 204)
        rep += "Content-Length: " + std::to_string(corps.size()) + "\r\n";
    rep += "Connection: close\r\n";
    rep += "\r\n";
    if (envoyer_corps && code != 204)
        rep += corps;
    return rep;
}

// recoit toute la requete du client
// recv peut donner la requete en plusieurs morceaux, donc on boucle :
//  1) jusqu'a trouver "\r\n\r\n" (fin des headers)
//  2) ensuite on lit le corps selon le Content-Length (utile pour PUT)
// retourne false si le client a ferme avant d'envoyer une requete complete
bool lire_requete(SOCKET client, std::string& entetes, std::string& corps)
{
    char recvbuf[DEFAULT_BUFLEN];
    std::string tout;
    size_t fin_entetes;

    while ((fin_entetes = tout.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(client, recvbuf, DEFAULT_BUFLEN, 0);
        if (n <= 0)
            return false;
        tout.append(recvbuf, n);
    }

    // on separe les headers et le debut du corps
    entetes = tout.substr(0, fin_entetes + 2);
    corps = tout.substr(fin_entetes + 4);

    std::string longueur = get_header(entetes, "Content-Length");
    size_t attendu = longueur.empty() ? 0 : (size_t)atol(longueur.c_str());
    while (corps.size() < attendu) {
        int n = recv(client, recvbuf, DEFAULT_BUFLEN, 0);
        if (n <= 0)
            break;
        corps.append(recvbuf, n);
    }
    if (corps.size() > attendu)
        corps.resize(attendu);
    return true;
}

// le coeur du serveur : lit la requete, regarde la methode
// et retourne la bonne reponse avec le bon code de statut
std::string traiter(const std::string& entetes, const std::string& corps)
{
    // premiere ligne : METHODE CHEMIN VERSION
    std::string ligne = entetes.substr(0, entetes.find("\r\n"));
    size_t e1 = ligne.find(' ');
    size_t e2 = (e1 == std::string::npos) ? std::string::npos : ligne.find(' ', e1 + 1);
    if (e1 == std::string::npos || e2 == std::string::npos)
        return faire_reponse(400, "Bad Request", page("400 Bad Request"), true);

    std::string methode = ligne.substr(0, e1);
    std::string chemin = ligne.substr(e1 + 1, e2 - e1 - 1);
    std::string version = ligne.substr(e2 + 1);

    // affiche la requete recue dans la console du serveur
    printf("%s %s %s\n", methode.c_str(), chemin.c_str(), version.c_str());

    // si ce n'est pas du HTTP, la requete est mal formee

    if (version.compare(0, 5, "HTTP/") != 0)
        return faire_reponse(400, "Bad Request", page("400 Bad Request"), true);

    if (version != "HTTP/1.1" && version != "HTTP/1.0")
        return faire_reponse(505, "HTTP Version Not Supported", page("505 HTTP Version Not Supported"), true);

    // en HTTP/1.1 le header Host est obligatoire
    if (chemin.empty() || chemin[0] != '/' || (version == "HTTP/1.1" && get_header(entetes, "Host").empty()))
        return faire_reponse(400, "Bad Request", page("400 Bad Request"), true);

    if (methode == "GET" || methode == "HEAD") {
        bool corps_ok = (methode == "GET"); // HEAD = meme chose que GET mais sans le corps
        if (ressources.count(chemin) == 0)
            return faire_reponse(404, "Not Found", page("404 Not Found"), corps_ok);
        return faire_reponse(200, "OK", ressources[chemin], corps_ok);
    }

    // PUT : cree le fichier (201) ou le remplace s'il existait deja (204)
    if (methode == "PUT") {
        bool existe = ressources.count(chemin) > 0;
        ressources[chemin] = corps;
        if (existe)
            return faire_reponse(204, "No Content", "", false);
        return faire_reponse(201, "Created", page("Fichier " + chemin + " cree"), true);
    }

    // DELETE : supprime le fichier (200) ou 404 s'il n'existe pas
    if (methode == "DELETE") {
        if (ressources.count(chemin) == 0)
            return faire_reponse(404, "Not Found", page("404 Not Found"), true);
        ressources.erase(chemin);
        return faire_reponse(200, "OK", page("Fichier " + chemin + " supprime"), true);
    }

    // POST, PATCH, etc. ne sont pas geres
    return faire_reponse(405, "Method Not Allowed", page("405 Method Not Allowed"), true,
                         "Allow: GET, HEAD, PUT, DELETE\r\n");
}

// point d'entree du serveur (appele par main.cpp)
void raylib_start(void)
{
    if (init_server() < 0) {
        printf("Failed to initialize server\n");
        exit(1);
    }

    // fichiers de depart pour pouvoir tester GET tout de suite
    ressources["/"] = page("Bienvenue sur mon serveur");
    ressources["/index.html"] = ressources["/"];

    printf("Server started on http://127.0.0.1:%s\n", DEFAULT_PORT);
    fflush(stdout);

    // une connexion = une requete = une reponse, puis on ferme
    while (true) {
        // accept bloque jusqu'a ce qu'un client se connecte
        SOCKET ClientSocket = accept(ListenSocket, NULL, NULL);
        if (ClientSocket == INVALID_SOCKET) {
            printf("accept failed: %d\n", (int)WSAGetLastError());
            continue;
        }

        std::string entetes, corps;
        if (lire_requete(ClientSocket, entetes, corps)) {
            std::string reponse = traiter(entetes, corps);
            int iSendResult = send(ClientSocket, reponse.c_str(), (int)reponse.size(), 0);
            if (iSendResult == SOCKET_ERROR)
                printf("send failed: %d\n", (int)WSAGetLastError());
            printf("-> %s\n", reponse.substr(0, reponse.find("\r\n")).c_str());
            fflush(stdout);
        }
        closesocket(ClientSocket);
    }
}
