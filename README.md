## Perceptual Image Hashing

This code calculates perceptual hashes of images, which are short (relative to the size of the image) sequences of numbers that are similar for similar-looking images. These hashes may be used for finding duplicate or very similar images in a large dataset.

There are two hashing methods, a block-rank algorithm and a DCT based algorithm. Both operate on a pre-processed image, which is the input image scaled to 128x128 pixels, histogram equalized, and converted to grayscale.

The block-rank algorithm further reduces the image to 20x20 pixels, and folds the four quadrants of the image in to produce a mirror-symmetrical 10x10 image. The hash's 64 bits correspond to the central 8x8 pixels of this image. Each is ranked relative to its neighbors, and if greater than half the corresponding bit is set, otherwise it is zero.

The DCT based algorithm simply computes the 2D DCT of the pre-processed image, discarding the 0-frequency and all odd-frequency components. Each bit of the hash is set if the corresponding DCT coefficient is positive. The bits of the hash are ordered such that including fewer DCT terms produces a prefix of the larger hash.

This fork of the upstream image-hash project aims to create a minimal implementation of the core perceptual hashing algorithms suitable for inclusion in other projects.  (The specific goal is to implement approximate image matching for regression testing of a rendering system - we want to catch large errors but not fail if single pixels differ.)
