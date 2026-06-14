#include "GridHelper.h"
#include <algorithm>
#include <cstdint>

uint16_t GridHelper::rowForIndex(uint16_t index) {
  return (index / cols) % rowsPerPage;
}

uint8_t GridHelper::colForIndex(uint16_t index) {
  return index % cols;
}

uint8_t GridHelper::currentRow() {
  return rowForIndex(index);
};

uint8_t GridHelper::currentCol() {
  return colForIndex(index);
};

uint8_t GridHelper::pageForIndex(uint16_t index) {
  return index / itemsPerPage();
}

uint8_t GridHelper::currentPage() {
  return pageForIndex(index);
};

void GridHelper::prevItem() {
  if(index == 0) {
    index = itemCount - 1;
  } else {
    index--;
  }
}

void GridHelper::nextItem() {
  index = (index + 1) % itemCount;
}

void GridHelper::up() {
  if(index < cols) {
    setByRowColPage(rowsOnFinalPage() - 1, currentCol(), pageCount() - 1);
    return;
  }

  index -= cols;
}

void GridHelper::down() {
  bool isFinalPage = currentPage() == (pageCount() - 1);
  bool shouldWrap = isFinalPage && currentRow() == (rowsOnFinalPage() - 1);

  if(shouldWrap) {
    setByRowColPage(0, currentCol(), 0);
    return;
  }

  setByIndex(index + cols);
};

void GridHelper::left() {
  if(currentCol() == 0) {
    index = std::min<uint16_t>(index + cols - 1, itemCount - 1);
  } else {
    index--;
  }
};

void GridHelper::right() {
  if(currentCol() == (cols - 1) || index == (itemCount - 1)) {
    setByRowColPage(currentRow(), 0, currentPage());
  } else {
    index++;
  }
};

void GridHelper::setByRowColPage(uint8_t row, uint8_t col, uint8_t page) {
  uint16_t index = page * itemsPerPage() + (row * cols) + col;
  setByIndex(index);
};

void GridHelper::setByIndex(uint16_t index) {
  // `index` is also the member name — must qualify the assignment target, or it
  // writes the parameter and leaves the member unchanged (a silent no-op).
  this->index = std::max<uint16_t>(0, std::min<uint16_t>(itemCount - 1, index));
};

uint8_t GridHelper::itemsPerPage() {
  return cols * rowsPerPage;
}

uint8_t GridHelper::pageCount() {
  if (itemCount == 0) return 0;
  return (itemCount + itemsPerPage() - 1) / itemsPerPage();
}

uint8_t GridHelper::rowsOnFinalPage() {
  if (itemCount == 0) return 0;
  uint16_t itemsOnFinal = ((itemCount - 1) % itemsPerPage()) + 1;
  return (itemsOnFinal + cols - 1) / cols;
}
