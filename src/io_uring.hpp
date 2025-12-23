#include <liburing.h>
#include <iostream>

/* io_uring simulates kernel bypass for incoming network operations. instead of system calls for network operations, the packets go straight to userspace
using kernel: NIC -> kernel copy -> syscall -> userspace app/buffer
using io_uring: network input -> userspace buffer

*/


int main() {
    io_uring ring;

    if (io_uring_queue_init(8, &ring, 0) < 0) { // init ring
        std::cerr << "io_uring init failed" << std::endl;
        return 1;
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&ring); // submission queue entry
    if (!sqe) {
        std::cerr << "io_uring_get_sqe failed" << std::endl;
        return 1;
    }

    io_uring_prep_nop(sqe); // no operation

    int ret = io_uring_submit(&ring); // submit sqe
    if (ret < 0) {
        std::cer << "io_uring_submit failed: " << ret << std::endl;
        return 1;
    }

    io_uring_cqe* cqe; // completion queue entry (event completion)
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        std::cerr << "io_uring_wait_cqe failed" << std::endl;
        return 1;
    }

    std::cout << "CQE res = " << cqe->res << std::endl;

    io_uring_cqe_seen(&ring, cqe); // mark cqe as seen

    io_uring_queue_exit(&ring); // cleanup

    return 0;
}