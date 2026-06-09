#include "GridHelper.h"
#include <algorithm>
#include <cstdint>

uint16_t GridHelper::rowForIndex(uint16_t index) {
  return (this->index / this->cols) % this->rowsPerPage;
}

uint8_t GridHelper::colForIndex(uint16_t index) {
  return this->index % this->cols;
}

uint8_t GridHelper::currentRow() {
  return this->rowForIndex(this->index);
};

uint8_t GridHelper::currentCol() {
  return this->colForIndex(this->index);
};

uint8_t GridHelper::currentPage() {
  return this->index / this->itemsPerPage();
};

void GridHelper::nextItem() {
  this->index = (this->index + 1) % this->itemCount;
}

void GridHelper::up() {
  if(this->index < this->cols) {
    this->setByRowColPage(this->rowsOnFinalPage() - 1, this->currentCol(), this->pageCount() - 1);
    return;
  }

  this->index -= this->cols;
}

void GridHelper::down() {
  bool isFinalPage = currentPage() == (pageCount() - 1);
  bool shouldWrap = isFinalPage && currentRow() == (rowsOnFinalPage() - 1);

  if(shouldWrap) {
    this->setByRowColPage(0, this->currentCol(), 0);
    return;
  }

  setByIndex(index + cols);
};

void GridHelper::left() {
  uint8_t currentCol = this->currentCol();

  if(currentCol == 0) {
    this->index = std::min<uint16_t>(this->index + this->cols - 1, this->itemCount);
  } else {
    this->index--;
  }
};

void GridHelper::right() {
  uint8_t currentCol = this->currentCol();

  if(currentCol == (this->cols - 1) || this->index == (this->itemCount - 1)) {
    this->setByRowColPage(this->currentRow(), 0, this->currentPage());
  } else {
    this->index++;
  }
};

void GridHelper::setByRowColPage(uint8_t row, uint8_t col, uint8_t page) {
  uint16_t index = page * this->itemsPerPage() + (row * this->cols) + col;
  this->setByIndex(index);
};

void GridHelper::setByIndex(uint16_t index) {
  this->index = std::max<uint16_t>(
      0,
      std::min<uint16_t>(this->itemCount - 1, index) 
  );
};

uint8_t GridHelper::itemsPerPage() {
  return this->cols * this->rowsPerPage;
}

uint8_t GridHelper::pageCount() {
  if (this->itemCount == 0) return 0;
  return (this->itemCount + this->itemsPerPage() - 1) / this->itemsPerPage();
}

uint8_t GridHelper::rowsOnFinalPage() {
  if (this->itemCount == 0) return 0;
  uint16_t itemsOnFinal = ((this->itemCount - 1) % this->itemsPerPage()) + 1;
  return (itemsOnFinal + this->cols - 1) / this->cols;
}
