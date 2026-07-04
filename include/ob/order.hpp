#pragma once

#include "ob/types.hpp"

namespace ob {

// A single resting order living inside a price level's FIFO queue.
// `quantity` is the *remaining* (unfilled) size; it shrinks as the order is
// hit and the order is erased when it reaches zero.
struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity quantity;
};

} // namespace ob
