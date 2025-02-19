#include <networkclient.h>

/*
 * This class provides interaction with the server
 */

NetworkClient::NetworkClient(QObject *parent) : QObject(parent)  { // An empty constructor is needed to create the object before placing it into the thread. Properties are initialized in initialized() outside the GUI thread

}

bool NetworkClient::isWebSocketConnected() {
    return webSocketConnected;
}

void NetworkClient::initialize() {
    webSocketConnecting = false;
    webSocketConnected = false;
    pendingWsMessages = new QVector<WsMessage*>();
    sendPendingWsMessagesTimer = new QTimer();
    webSocket = new QWebSocket();
    authorizationManager = new AuthorizationManager();
    networkManager = new QNetworkAccessManager();
    sendPendingWsMessagesTimer->start(10000);
    QObject::connect(sendPendingWsMessagesTimer, &QTimer::timeout, this, &NetworkClient::sendPendingWsMessages);
    QObject::connect(webSocket, &QWebSocket::connected, this, &NetworkClient::webSocketConnectedSlot);
    QObject::connect(webSocket, &QWebSocket::disconnected, this, &NetworkClient::webSocketDisconnected);
    QObject::connect(webSocket, &QWebSocket::textMessageReceived, this, &NetworkClient::webSocketMessageReceivedSlot);
    emit initialized();
}

void NetworkClient::webSocketMessageReceivedSlot(const QString &message) {
    QJsonObject data = QJsonDocument::fromJson(message.toUtf8()).object();
    if (data["method"] == "acknowledged") { // Acknowledged is set when the server receives and processes the message
        QString tempId = data["tempId"].toString(); // < temp ID for the message
        for (int i = 0; i < pendingWsMessages->size(); ++i) {
            WsMessage *current = pendingWsMessages->at(i);
            if (current->getTempId() == tempId) {
                pendingWsMessages->remove(i);
                break;
            }
        }
    }
    else {
        emit webSocketMessageReceived(data);
    }
}

void NetworkClient::sendPendingWsMessages() {
    QDateTime utcDateTime = QDateTime::currentDateTimeUtc();
    qlonglong msecs = utcDateTime.toMSecsSinceEpoch();
    for (int i = 0; i < pendingWsMessages->size(); ++i) {
        WsMessage *current = pendingWsMessages->at(i);
        if (current->getCreatedAt() + 10000 < msecs) {
            if (webSocketConnected) {
                webSocket->sendTextMessage(current->getMessage());
            }
        }
    }
}

// Called when a connection to the server is established.
void NetworkClient::webSocketConnectedSlot() {
    webSocketConnecting = false;
    webSocketConnected = true;
    emit webSocketConnectedSignal();
}

void NetworkClient::sendMessage(QString message) {
    QDateTime utcDateTime = QDateTime::currentDateTimeUtc();
    qlonglong msecs = utcDateTime.toMSecsSinceEpoch();
    QString tempId = QUuid::createUuid().toString();
    message += "\"time\": " + QString::number(msecs) +  ", \"tempId\": \"" + tempId + "\"}";
    WsMessage *newMessage = new WsMessage(msecs, message, tempId);
    pendingWsMessages->push_back(newMessage);
    if (webSocketConnected) {
        webSocket->sendTextMessage(message);
    }
}

void NetworkClient::connectWebSocket() {
    webSocketConnecting = true;
    QNetworkRequest request(QUrl("ws://localhost:8080/websocket/connect"));
    setAuthorizationHeader(request);
    webSocket->open(request);
    QTimer::singleShot(5000, this, [this]() {
        if (!webSocketConnected) {
           webSocket->abort();
           connectWebSocket();
        }
    });
}

void NetworkClient::webSocketDisconnected() {
    webSocketConnected = false;
    emit webSocketDisconnectedSignal();
    if (!webSocketConnecting) {
        connectWebSocket();
    }
}

// The "path" parameter accepts either "sign/in" or "sign/up".
void NetworkClient::sign(QMap<QString, QString> body, QString path) {
    QNetworkRequest request = createHttpRequest(path);
    QNetworkReply *reply = networkManager->post(request, formContent(body));
    auto success = QSharedPointer<bool>::create(false);
    auto statusChecked = QSharedPointer<bool>::create(false);
    auto dataAsArray = QSharedPointer<QByteArray>::create(QByteArray());
    QObject::connect(reply, &QNetworkReply::readyRead, this, [success, statusChecked, dataAsArray, reply, this, path]() {
        if (!*statusChecked) {
            *statusChecked = true;
            QString status = QString::fromUtf8(reply->readAll()).simplified();
            status.replace("data:", "");
            if (status == "Not Found") {
                emit httpSignError("User not found!");
                return;
            }
            else if (status == "Forbidden") {
                emit httpSignError("Wrong password!");
                return;
            }
            else if (status == "Conflict") {
                emit httpSignError("User already exists!");
                return;
            }
            else if (status == "Unprocessable Entity") {
                emit httpSignError("Something went wrong, try again!");
                return;
            }
            else {
                *success = true;
                emit shouldConfirmEmailSignal();
            }
        }
        else {
            dataAsArray->append(reply->readAll());
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [body, path, success, dataAsArray, reply, this]() {
        if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
            emit httpSignError("We are experiencing some issues on our server!");
            sign(body, path);
            return ;
        }
        if (*success) {
            QByteArray nonPointerData = *dataAsArray;
            nonPointerData = nonPointerData.replace("data:", "");
            nonPointerData = nonPointerData.simplified();
            QJsonDocument dataAsDocument = QJsonDocument::fromJson(nonPointerData);
            QJsonObject data = dataAsDocument.object();
            authorizationManager->setBothTokens(data["access"].toString(), data["refresh"].toString());
            emit httpSignProcessed();
        }
        reply->deleteLater();
    });
}


// The conditions for validating the refresh token are the same as in the refresh() method, except that this method also checks whether the token has expired. Called during initialization
void NetworkClient::checkRefreshToken() {
    QEventLoop *eventLoop = new QEventLoop();
    QMap<QString, QString> body;
    body.insert("refresh", authorizationManager->getRefreshToken());
    QNetworkRequest request = createHttpRequest(body, "sign/check-refresh");
    QNetworkReply *reply = networkManager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        checkRefreshToken();
        return ;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        emit unauthorizedSignal();
    }
    else {
        emit httpSignProcessed();
    }
}

void NetworkClient::createGroup(QMap<QString, QString> body) {
    QNetworkRequest request = createHttpRequest("create/group");
    if (!setAuthorizationHeader(request)) {
        emit unauthorizedSignal();
        return ;
    }
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkReply *reply = networkManager->post(request, formContent(body));
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        createGroup(body);
        return ;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 409) {
        emit createGroupError("This group already exists!");
    }
    else {
        QByteArray dataAsArray = reply->readAll();
        QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
        emit createGroupProcessed(dataAsDocument.object());
    }
}

void NetworkClient::findChats(QMap<QString, QString> body) {
    QNetworkRequest request = createHttpRequest(body, "api/find");
    if (!setAuthorizationHeader(request)) {
        emit unauthorizedSignal();
        return ;
    }
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkReply *reply = networkManager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        findChats(body);
        return ;
    }
    QByteArray dataAsArray = reply->readAll();
    QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
    emit findChatsProcessed(dataAsDocument.array());
}

void NetworkClient::getYourChats() {
    QNetworkRequest request = createHttpRequest("api/chats");
    if (!setAuthorizationHeader(request)) {
        emit unauthorizedSignal();
        return ;
    }
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkReply *reply = networkManager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        getYourChats();
        return ;
    }
    QByteArray dataAsArray = reply->readAll();
    QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
    emit getYourChatsProcessed(dataAsDocument.array());
}

void NetworkClient::getDialogueMessages(QMap<QString, QString> body) {
    QNetworkRequest request = createHttpRequest(body, "messages/dialogue");
    if (!setAuthorizationHeader(request)) {
        emit unauthorizedSignal();
        return ;
    }
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkReply *reply = networkManager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        getDialogueMessages(body);
        return ;
    }
    QByteArray dataAsArray = reply->readAll();
    QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
    emit getDialogueMessagesProcessed(dataAsDocument.array(), body["otherId"].toLongLong(), !body.contains("lastMessageId"));
}

void NetworkClient::getGroupMessages(QMap<QString, QString> body) {
    QNetworkRequest request = createHttpRequest(body, "messages/group");
    if (!setAuthorizationHeader(request)) {
        emit unauthorizedSignal();
        return ;
    }
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkReply *reply = networkManager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        getGroupMessages(body);
        return ;
    }
    QByteArray dataAsArray = reply->readAll();
    QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
    emit getGroupMessagesProcessed(dataAsDocument.array(), body["groupId"].toLongLong(), !body.contains("lastMessageId"));
}

/*
 * The conditions for validating the refresh token are the same as in the checkRefreshToken() method,
 * except that the token's expiration date is not checked—otherwise, a user with a valid but expired token
 * could be logged out right in the middle of a session. This method triggers the UnauthorizedSignal only
 * if the token is invalid. Called during program execution
 */
bool NetworkClient::refresh(QMap<QString, QString> body) {
    QEventLoop *eventLoop = new QEventLoop();
    QNetworkRequest request = createHttpRequest("sign/refresh");
    QNetworkReply *reply = networkManager->post(request, formContent(body));
    QObject::connect(reply, &QNetworkReply::finished, eventLoop, &QEventLoop::quit);
    eventLoop->exec();
    delete eventLoop;
    if (reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::ConnectionRefusedError) {
        return true;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        return false;
    }
    QByteArray dataAsArray = reply->readAll();
    QJsonDocument dataAsDocument = QJsonDocument::fromJson(dataAsArray);
    QJsonObject data = dataAsDocument.object();
    authorizationManager->setAccessToken(data["access"].toString());
    return true;
}

bool NetworkClient::setAuthorizationHeader(QNetworkRequest &request) {
    if (authorizationManager->isAccessTokenExpired()) {
        QMap<QString, QString> body;
        body.insert("refresh", authorizationManager->getRefreshToken());
        if (!refresh(body)) {
            return false;
        }
    }
    request.setRawHeader("Authorization", "Bearer " + authorizationManager->getAccessToken().toUtf8());
    return true;
}

QNetworkRequest NetworkClient::createHttpRequest(QString path) {
    QNetworkRequest request = QNetworkRequest(QUrl("http://localhost:8080/" + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    return request;
}

QByteArray NetworkClient::formContent(QMap<QString, QString> body) {
    return toUrlEncoded(body).toUtf8();
}

QString NetworkClient::toUrlEncoded(QMap<QString, QString> body) {
    QString result = "";
    for (QMap<QString, QString>::iterator iter = body.begin(); iter != body.end(); ++iter) {
        if (iter != body.begin()) {
            result.append("&");
        }
        result.append(iter.key());
        result.append("=");
        result.append(iter.value());
    }
    return result;
}

QNetworkRequest NetworkClient::createHttpRequest(QMap<QString, QString> body, QString path) {
    QString url = "http://localhost:8080/" + path + "?";
    url.append(toUrlEncoded(body));
    QNetworkRequest request = QNetworkRequest(QUrl(url));
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, QVariant("application/x-www-form-urlencoded"));
    return request;
}

void NetworkClient::deleteThis() {
    delete this;
}

NetworkClient::~NetworkClient() {
    if (webSocket->isValid()) {
        webSocket->close();
    }
    else {
        webSocket->abort();
    }
    delete authorizationManager;
    this->thread()->quit();
}
