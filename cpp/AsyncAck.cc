#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <iostream>
#include <iomanip>
#include <functional>
#include <cstdint>
#include <chrono>
#include <thread>

namespace
{
  //
  // to help freeing C resources
  //
  struct on_destruct
  {
    std::function<void()> fun_;
    on_destruct(std::function<void()> fun) : fun_(fun) {}
    ~on_destruct()
    {
      if (fun_)
        fun_();
    }
  };

  //
  // for measuring ellapsed time and print statistics
  //
  struct timer
  {
    typedef std::chrono::high_resolution_clock highres_clock;
    typedef std::chrono::time_point<highres_clock> timepoint;

    timepoint start_;
    uint64_t iteration_;

    timer(uint64_t iter) : start_{highres_clock::now()}, iteration_{iter} {}

    int64_t spent_usec()
    {
      using namespace std::chrono;
      timepoint now{highres_clock::now()};
      return duration_cast<microseconds>(now - start_).count();
    }

    ~timer()
    {
      using namespace std::chrono;
      timepoint now{highres_clock::now()};

      uint64_t usec_diff = duration_cast<microseconds>(now - start_).count();
      double call_per_ms = iteration_ * 1000.0 / ((double)usec_diff);
      double call_per_sec = iteration_ * 1000000.0 / ((double)usec_diff);
      double us_per_call = (double)usec_diff / (double)iteration_;

      std::cout << "elapsed usec=" << usec_diff
                << " avg(usec/call)=" << std::setprecision(8) << us_per_call
                << " avg(call/msec)=" << std::setprecision(8) << call_per_ms
                << " avg(call/sec)=" << std::setprecision(8) << call_per_sec
                << std::endl;
    }
  };

  template <size_t MAX_ITEMS>
  struct buffer
  {
    // each packet has 3 parts:
    // - 64 bit ID
    // - 32 bit size
    // - data
    struct iovec items_[MAX_ITEMS];
    uint64_t ids_[MAX_ITEMS];
    size_t n_items_;
    uint32_t len_;
    char data_[5];

    buffer() : n_items_{0}, len_{5}
    {

      // send an emtpy jason object
      memcpy(data_, "{} \r\n", 5);

      for (size_t i = 0; i < MAX_ITEMS; ++i)
      {
        // I am cheating with the packet content to be fixed
        // to "hello", but for the purpose of this test app
        // it is OK.
        //
        ids_[i] = 0;
        items_[i].iov_base = data_;
        items_[i].iov_len = len_;
      }
    }

    void push(uint64_t id)
    {
      ids_[n_items_++] = id;
    }

    bool needs_flush() const
    {
      return (n_items_ >= MAX_ITEMS);
    }

    void flush(int sockfd)
    {
      if (!n_items_)
        return;

      if (writev(sockfd, items_, n_items_) != (5 * n_items_))
      {
        throw "failed to send data";
      }

      n_items_ = 0;
    }
  };
} // namespace

int main(int argc, char **argv)
{
  try
  {
    // create a TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
      throw "can't create socket";
    }
    on_destruct close_sockfd([sockfd]() { close(sockfd); });

    // server address (127.0.0.1:8005)
    struct sockaddr_in server_addr;
    ::memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(8005);

    // connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
    {
      throw "failed to connect to server at 127.0.0.1:8005";
    }

    {
      /* This hurts performance
      int flag = 1;
      if( setsockopt( sockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag)) == -1 )
      {
        throw "failed to set TCP_NODELAY on the socket";
      }
      */
    }

    // the buffer template parameter tells how many messages shall
    // we batch together
    buffer<150> data;
    uint64_t id = 0;
    int64_t last_ack = -1;

    //
    // this lambda function checks if we have received a new ACK.
    // if we did then it checks the content and returns the max
    // acknowledged ID. this supports receiving multiple ACKs in
    // a single transfer.
    //
    auto check_ack = [sockfd](int64_t last_ack) {
      int64_t ret_ack = last_ack;
      fd_set fdset;
      FD_ZERO(&fdset);
      FD_SET(sockfd, &fdset);

      // give 1 msec to the acks to arrive
      struct timeval tv
      {
        0, 1000
      };
      int select_ret = select(sockfd + 1, &fdset, NULL, NULL, &tv);
      if (select_ret < 0)
      {
        throw "failed to select, socket error?";
      }
      else if (select_ret == 0)
      {
        // timed out
      }
      else if (select_ret > 0 && FD_ISSET(sockfd, &fdset))
      {
        // max 2048 acks that we handle in one check
        size_t alloc_bytes = 12 * 2048;
        std::unique_ptr<uint8_t[]> ack_data{new uint8_t[alloc_bytes]};

        //
        // let's receive what has arrived. if there are more than 2048
        // ACKs waiting, then the next loop will take care of them
        //

        auto recv_ret = recv(sockfd, ack_data.get(), alloc_bytes, 0);
        if (recv_ret < 0)
        {
          throw "failed to recv, socket error?";
        }
        else if (recv_ret < 12)
        {
          throw "failed to recv, partial packet?";
        }
        else if (recv_ret > 11)
        {
          for (size_t pos = 0; pos < recv_ret; pos += 12)
          {
            uint64_t id = 0;
            uint32_t skipped = 0;

            // copy the data to the variables above
            //
            memcpy(&id, ack_data.get() + pos, sizeof(id));
            memcpy(&skipped, ack_data.get() + pos + sizeof(id), sizeof(skipped));

            // check the ACKs
            if ((ret_ack + skipped + 1) != id)
            {
              std::cerr << "ret_ack=" << ret_ack << " skipped=" << skipped << " recvd_id=" << id << " missing=" << id - ret_ack - skipped << std::endl;
              throw "missing ack";
            }
            ret_ack = id;
          }
        }
      }
      return ret_ack;
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (int i = 0; i < 100; ++i)
    {
      size_t iter = 10;
      timer t(iter);
      int64_t checked_at_usec = 0;

      // send data in a loop
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      for (size_t kk = 0; kk < iter; ++kk)
      {
        data.push(id);
        data.flush(sockfd);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++id;
      }

      // wait for all outstanding ACKs
      while (last_ack < (id - 1))
      {
        last_ack = check_ack(last_ack);
        // time out in 20 seconds
        if (t.spent_usec() > 20000000)
        {
          std::cerr << "last_ack=" << last_ack << " id=" << id - 1 << " missing=" << (id - 1) - last_ack << std::endl;
          throw "timed out while waiting for ACK";
        }
      }
    }

    while (last_ack < (id - 1))
    {
      last_ack = check_ack(last_ack);
      if (last_ack != id)
      {
        std::cerr << "last_ack=" << last_ack << " id=" << id << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
  }
  catch (const char *msg)
  {
    perror(msg);
  }
  return 0;
}
