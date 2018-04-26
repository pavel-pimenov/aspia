//
// PROJECT:         Aspia
// FILE:            network/network_channel.cc
// LICENSE:         GNU General Public License 3
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "network/network_channel.h"

#include <QtEndian>

#include "crypto/encryptor.h"

namespace aspia {

namespace {

constexpr quint32 kMaxMessageSize = 16 * 1024 * 1024; // 16MB
constexpr int kReadBufferReservedSize = 128 * 1024; // 128kB
constexpr qint64 kMaxWriteSize = 1400;

QByteArray createWriteBuffer(const QByteArray& message_buffer)
{
    quint32 message_size = message_buffer.size();

    quint8 buffer[4];
    size_t length = 1;

    buffer[0] = message_size & 0x7F;
    if (message_size > 0x7F) // 127 bytes
    {
        buffer[0] |= 0x80;
        buffer[length++] = message_size >> 7 & 0x7F;

        if (message_size > 0x3FFF) // 16383 bytes
        {
            buffer[1] |= 0x80;
            buffer[length++] = message_size >> 14 & 0xFF;

            if (message_size > 0x1FFFF) // 2097151 bytes
            {
                buffer[2] |= 0x80;
                buffer[length++] = message_size >> 21 & 0xFF;
            }
        }
    }

    QByteArray write_buffer;
    write_buffer.resize(length + message_size);

    memcpy(write_buffer.data(), buffer, length);
    memcpy(write_buffer.data() + length, message_buffer.constData(), message_size);

    return write_buffer;
}

} // namespace

NetworkChannel::NetworkChannel(ChannelType channel_type, QTcpSocket* socket, QObject* parent)
    : QObject(parent),
      channel_type_(channel_type),
      socket_(socket)
{
    Q_ASSERT(!socket_.isNull());

    socket_->setParent(this);

    if (channel_type_ == ClientChannel)
        connect(socket_, &QTcpSocket::connected, this, &NetworkChannel::onConnected);

    connect(socket_, &QTcpSocket::disconnected, this, &NetworkChannel::disconnected);
    connect(socket_, &QTcpSocket::bytesWritten, this, &NetworkChannel::onBytesWritten);
    connect(socket_, &QTcpSocket::readyRead, this, &NetworkChannel::onReadyRead);

    connect(socket_, QOverload<QTcpSocket::SocketError>::of(&QTcpSocket::error),
            this, &NetworkChannel::onError);

    read_buffer_.reserve(kReadBufferReservedSize);
}

NetworkChannel::~NetworkChannel()
{
    stop();
}

// static
NetworkChannel* NetworkChannel::createClient(QObject* parent)
{
    return new NetworkChannel(ClientChannel, new QTcpSocket(), parent);
}

void NetworkChannel::connectToHost(const QString& address, int port)
{
    if (channel_type_ == ServerChannel)
    {
        qWarning("The channel is server. The method invocation is invalid.");
        return;
    }

    if (socket_.isNull())
        return;

    socket_->connectToHost(address, port);
}

void NetworkChannel::readMessage()
{
    Q_ASSERT(!read_required_);

    read_required_ = true;
    onReadyRead();
}

void NetworkChannel::writeMessage(int message_id, const QByteArray& buffer)
{
    if (!encryptor_)
    {
        qWarning("Uninitialized encryptor");
        return;
    }

    write(message_id, encryptor_->encrypt(buffer));
}

void NetworkChannel::stop()
{
    channel_state_ = NotConnected;

    if (!socket_.isNull())
    {
        socket_->abort();

        if (socket_->state() != QTcpSocket::UnconnectedState)
            socket_->waitForDisconnected();
    }
}

void NetworkChannel::onConnected()
{
    channel_state_ = Connected;

    // Disable the Nagle algorithm for the socket.
    socket_->setSocketOption(QTcpSocket::LowDelayOption, 1);

    if (channel_type_ == ServerChannel)
    {
        encryptor_.reset(new Encryptor(Encryptor::ServerMode));

        // Start reading hello message.
        readMessage();
    }
    else
    {
        Q_ASSERT(channel_type_ == ClientChannel);
        encryptor_.reset(new Encryptor(Encryptor::ClientMode));

        // Write hello message to server.
        write(-1, encryptor_->helloMessage());
    }
}

void NetworkChannel::onError(QAbstractSocket::SocketError /* error */)
{
    emit errorOccurred(socket_->errorString());
}

void NetworkChannel::onBytesWritten(qint64 bytes)
{
    if (socket_.isNull())
    {
        stop();
        return;
    }

    written_ += bytes;

    const QByteArray& write_buffer = write_queue_.front().second;

    if (written_ < write_buffer.size())
    {
        qint64 bytes_to_write = qMin(write_buffer.size() - written_, kMaxWriteSize);
        socket_->write(write_buffer.constData() + written_, bytes_to_write);
    }
    else
    {
        onMessageWritten(write_queue_.front().first);

        write_queue_.pop();
        written_ = 0;

        if (!write_queue_.empty())
            scheduleWrite();
    }
}

void NetworkChannel::onReadyRead()
{
    if (socket_.isNull())
    {
        stop();
        return;
    }

    if (!read_required_)
        return;

    qint64 current;

    for (;;)
    {
        if (!read_size_received_)
        {
            quint8 byte;

            current = socket_->read(reinterpret_cast<char*>(&byte), sizeof(byte));
            if (current == sizeof(byte))
            {
                switch (read_)
                {
                    case 0:
                        read_size_ += byte & 0x7F;
                        break;

                    case 1:
                        read_size_ += (byte & 0x7F) << 7;
                        break;

                    case 2:
                        read_size_ += (byte & 0x7F) << 14;
                        break;

                    case 3:
                        read_size_ += byte << 21;
                        break;
                }

                if (!(byte & 0x80) || read_ == 3)
                {
                    read_size_received_ = true;

                    if (!read_size_ || read_size_ > kMaxMessageSize)
                    {
                        qWarning() << "Wrong message size: " << read_size_;
                        stop();
                        return;
                    }

                    read_buffer_.resize(read_size_);
                    read_size_ = 0;
                    read_ = 0;
                    continue;
                }
            }
        }
        else if (read_ < read_buffer_.size())
        {
            current = socket_->read(read_buffer_.data() + read_, read_buffer_.size() - read_);
        }
        else
        {
            read_required_ = false;
            read_size_received_ = false;
            read_ = 0;

            onMessageReceived(read_buffer_);
            break;
        }

        if (current == 0)
            break;

        read_ += current;
    }
}

void NetworkChannel::onMessageWritten(int message_id)
{
    switch (channel_state_)
    {
        case Encrypted:
            emit messageWritten(message_id);
            break;

        case Connected:
        {
            if (channel_type_ == ServerChannel)
            {
                channel_state_ = Encrypted;
                emit connected();
            }
            else
            {
                Q_ASSERT(channel_type_ == ClientChannel);

                // Read hello message from server.
                readMessage();
            }
        }
        break;
    }
}

void NetworkChannel::onMessageReceived(const QByteArray& buffer)
{
    switch (channel_state_)
    {
        case Encrypted:
        {
            if (!encryptor_)
            {
                qWarning("Uninitialized encryptor");
                return;
            }

            emit messageReceived(encryptor_->decrypt(buffer));
        }
        break;

        case Connected:
        {
            if (!encryptor_->readHelloMessage(buffer))
            {
                stop();
                return;
            }

            if (channel_type_ == ServerChannel)
            {
                write(-1, encryptor_->helloMessage());
            }
            else
            {
                Q_ASSERT(channel_type_ == ClientChannel);

                channel_state_ = Encrypted;
                emit connected();
            }
        }
        break;
    }
}

void NetworkChannel::write(int message_id, const QByteArray& buffer)
{
    if (socket_.isNull() || buffer.isEmpty() || buffer.size() > kMaxMessageSize)
    {
        stop();
        return;
    }

    bool schedule_write = write_queue_.empty();

    write_queue_.emplace(message_id, createWriteBuffer(buffer));

    if (schedule_write)
        scheduleWrite();
}

void NetworkChannel::scheduleWrite()
{
    const QByteArray& write_buffer = write_queue_.front().second;
    socket_->write(write_buffer.constData(), write_buffer.size());
}

} // namespace aspia