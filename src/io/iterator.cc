#include "shale/iterator.h"

namespace shale {

// Defined here, and not inline in the header, so that the class's virtual table has one home.
Iterator::~Iterator() = default;

}  // namespace shale
