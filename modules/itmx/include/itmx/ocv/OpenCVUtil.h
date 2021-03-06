/**
 * itmx: OpenCVUtil.h
 * Copyright (c) Torr Vision Group, University of Oxford, 2015. All rights reserved.
 */

#ifndef H_ITMX_OPENCVUTIL
#define H_ITMX_OPENCVUTIL

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <ORUtils/Math.h>

namespace itmx {

/**
 * \brief This class provides helper functions to visualise image data using OpenCV.
 */
class OpenCVUtil
{
  //#################### ENUMERATIONS ####################
public:
  /**
   * \brief An enumeration containing two possible ways of arranging multidimensional arrays in a single linear array.
   */
  enum Order
  {
    COL_MAJOR,
    ROW_MAJOR
  };

  //#################### NESTED TYPES ####################
private:
  /**
   * \brief A functor that scales values by the specified factor.
   */
  struct ScaleByFactor
  {
    /** The factor by which to scale values. */
    float m_factor;

    /**
     * \brief Constructs a function that scales values by the specified factor.
     *
     * \param factor  The factor by which to scale values.
     */
    explicit ScaleByFactor(float factor)
    : m_factor(factor)
    {}

    /**
     * \brief Scales the specified value by the factor associated with this functor.
     *
     * \return  The result of scaling the specified value by the factor associated with this functor.
     */
    float operator()(float value) const
    {
      return value * m_factor;
    }
  };

  /**
   * \brief A functor that implements a linear mapping from an input range [minInputValue,maxInputValue] derived from an array of input data
   *        to the range [minOutputValue,maxOutputValue].
   */
  struct ScaleDataToRange
  {
    //~~~~~~~~~~~~~~~~~~~~ PUBLIC VARIABLES ~~~~~~~~~~~~~~~~~~~~

    /** The lower bound of the input range. */
    float m_minInputValue;

    /** The lower bound of the output range. */
    float m_minOutputValue;

    /** The ratio between the size of the output range and the size of the input range (e.g. if the output range is twice the size then this would equal 2). */
    float m_scalingFactor;

    //~~~~~~~~~~~~~~~~~~~~ CONSTRUCTORS ~~~~~~~~~~~~~~~~~~~~

    /**
     * \brief Constructs a functor that implements a linear mapping from the range [minInputValue,maxInputValue] derived from an array of input data
     *        to the range [minOutputValue,maxOutputValue].
     *
     * \param inputData       The array of input data from which to derive the input range.
     * \param inputDateSize   The size of the array of input data.
     * \param minOutputValue  The lower bound of the output range.
     * \param maxOutputValue  The upper bound of the output range.
     */
    ScaleDataToRange(const float *inputData, int inputDataSize, float minOutputValue, float maxOutputValue)
    : m_minOutputValue(minOutputValue)
    {
      const float *inputDataEnd = inputData + inputDataSize;
      float maxInputValue = *std::max_element(inputData, inputDataEnd);
      m_minInputValue = *std::min_element(inputData, inputDataEnd);
      m_scalingFactor = (maxOutputValue - minOutputValue) / (maxInputValue - m_minInputValue);
    }

    /**
     * \brief Maps the specified input value into the output range.
     *
     * \param inputValue  A value in the input range.
     * \return            The result of mapping the specified input value into the output range.
     */
    float operator()(float inputValue) const
    {
      return m_minOutputValue + (inputValue - m_minInputValue) * m_scalingFactor;
    }
  };

  //#################### PUBLIC STATIC MEMBER FUNCTIONS ####################
public:
  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format.
   *
   * \param inputData The pixel data for the image.
   * \param width     The width of the image.
   * \param height    The height of the image.
   * \param order     Whether the pixel data is in row-major or column-major order.
   * \return          The OpenCV image.
   */
  template <typename T>
  static cv::Mat1b make_greyscale_image(const T *inputData, int width, int height, Order order)
  {
    return make_greyscale_image(inputData, width, height, order, &identity<T>);
  }

  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format,
   *        applying the specified scaling factor to each pixel value as it goes.
   *
   * \param inputData   The pixel data for the image.
   * \param width       The width of the image.
   * \param height      The height of the image.
   * \param order       Whether the pixel data is in row-major or column-major order.
   * \param scaleFactor The scaling factor.
   * \return            The OpenCV image.
   */
  template <typename T>
  static cv::Mat1b make_greyscale_image(const T *inputData, int width, int height, Order order, float scaleFactor)
  {
    return make_greyscale_image(inputData, width, height, order, ScaleByFactor(scaleFactor));
  }

  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format,
   *        applying the specified scaling function to each pixel value as it goes.
   *
   * \param inputData The pixel data for the image.
   * \param width     The width of the image.
   * \param height    The height of the image.
   * \param order     Whether the pixel data is in row-major or column-major order.
   * \param scaleFunc The scaling function.
   * \return          The OpenCV image.
   */
  template <typename T, typename ScaleFunc>
  static cv::Mat1b make_greyscale_image(const T *inputData, int width, int height, Order order, ScaleFunc scaleFunc)
  {
    cv::Mat1b result = cv::Mat::zeros(height, width, CV_8UC1);
    unsigned char *outputData = result.data;

    int pixelCount = width * height;
    if(order == ROW_MAJOR)
    {
      for(int i = 0; i < pixelCount; ++i)
      {
        *outputData++ = clamp_pixel_value(scaleFunc(*inputData++));
      }
    }
    else // order == COL_MAJOR
    {
      for(int y = 0; y < height; ++y)
      {
        for(int x = 0; x < width; ++x)
        {
          *outputData++ = clamp_pixel_value(scaleFunc(inputData[x * height + y]));
        }
      }
    }

    return result;
  }

  /**
   * \brief Makes an RGB image of the specified size from some pixel data.
   *
   * \param rgbData The pixel data for the image, in the format [R1,G1,B1,R2,G2,B2,...].
   * \param width   The width of the image.
   * \param height  The height of the image.
   * \return        The image.
   */
  static cv::Mat3b make_rgb_image(const float *rgbData, int width, int height);

  /**
   * \brief Makes an RGB image of the specified size from some RGBA pixel data.
   *
   * Note: The alpha channel is discarded during this process.
   *
   * \param rgbaData  The pixel data for the image, in the format [(R1,G1,B1,A1),(R2,G2,B2,A2),...].
   * \param width     The width of the image.
   * \param height    The height of the image.
   * \return          The image.
   */
  static cv::Mat3b make_rgb_image(const Vector4u *rgbaData, int width, int height);

  /**
   * \brief Makes a copy of an RGB image that has been padded with a black border.
   *
   * \param image       The image to be padded.
   * \param paddingSize The size of the black border to be placed around the image (in pixels).
   */
  static cv::Mat3b pad_image(const cv::Mat3b& image, int paddingSize);

  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format and shows it in a named window.
   *
   * \param windowName  The name to give the window.
   * \param inputData   The pixel data for the image.
   * \param width       The width of the image.
   * \param height      The height of the image.
   * \param order       Whether the pixel data is in row-major or column-major order.
   */
  template <typename T>
  static void show_greyscale_figure(const std::string& windowName, const T *inputData, int width, int height, Order order)
  {
    show_scaled_greyscale_figure(windowName, inputData, width, height, order, &identity<T>);
  }

  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format, applying the specified scaling
   *        factor to each pixel value as it goes, and shows the resulting image in a named window.
   *
   * \param windowName  The name to give the window.
   * \param inputData   The pixel data for the image.
   * \param width       The width of the image.
   * \param height      The height of the image.
   * \param order       Whether the pixel data is in row-major or column-major order.
   * \param scaleFactor The scaling factor.
   */
  static void show_scaled_greyscale_figure(const std::string& windowName, const float *inputData, int width, int height, Order order, float scaleFactor);

  /**
   * \brief Makes a greyscale OpenCV image from some pixel data in the specified format, applying the specified scaling
   *        function to each pixel value as it goes, and shows the resulting image in a named window.
   *
   * \param windowName  The name to give the window.
   * \param inputData   The pixel data for the image.
   * \param width       The width of the image.
   * \param height      The height of the image.
   * \param order       Whether the pixel data is in row-major or column-major order.
   * \param scaleFunc   The scaling function.
   */
  template <typename T, typename ScaleFunc>
  static void show_scaled_greyscale_figure(const std::string& windowName, const T *inputData, int width, int height, Order order, ScaleFunc scaleFunc)
  {
    cv::imshow(windowName, make_greyscale_image(inputData, width, height, order, scaleFunc));
  }

  /**
   * \brief Tiles images on a regular grid.
   *
   * The tiles are generated by resizing the input images to create image patches and then padding each patch with a black border.
   * If the number of images to tile exceeds the number of cells in the grid, the surplus images will be ignored.
   *
   * \param images        The images to tile.
   * \param tileCols      The number of columns in the grid.
   * \param tileRows      The number of rows in the grid.
   * \param patchWidth    The width of each image patch (i.e. the width of an unpadded tile).
   * \param patchHeight   The height of each image patch (i.e. the height of an unpadded tile).
   * \param paddingSize   The size of the black border to be placed around each tile (in pixels).
   * \return              The tiled image.
   */
  static cv::Mat3b tile_image_patches(const std::vector<cv::Mat3b>& images, int tileCols, int tileRows, int patchWidth, int patchHeight, int paddingSize = 2);

  /**
   * \brief Tiles images on a regular grid within a overall image of fixed dimensions.
   *
   * The tiles are generated by resizing the input images to create image patches and then padding each patch with a black border.
   * If the number of images to tile exceeds the number that can fit within the overall image, the surplus images will be ignored.
   *
   * \param images        The images to tile.
   * \param imageWidth    The width of the overall image.
   * \param imageHeight   The height of the overall image.
   * \param patchWidth    The width of each image patch (i.e. the width of an unpadded tile).
   * \param patchHeight   The height of each image patch (i.e. the height of an unpadded tile).
   * \param paddingSize   The size of the black border to be placed around each tile (in pixels).
   * \return              The tiled image.
   */
  static cv::Mat3b tile_image_patches_bounded(const std::vector<cv::Mat3b>& images, int imageWidth, int imageHeight, int patchWidth, int patchHeight, int paddingSize = 2);

  //#################### PRIVATE STATIC MEMBER FUNCTIONS ####################
private:
  /**
   * \brief Clamps the specified pixel value to the range [0,255] and converts it to an unsigned char.
   *
   * \param pixelValue  The pixel value.
   * \return            The clamped pixel value as an unsigned char.
   */
  template <typename T>
  static unsigned char clamp_pixel_value(T pixelValue)
  {
    if(pixelValue < T(0)) pixelValue = T(0);
    if(pixelValue > T(255)) pixelValue = T(255);
    return static_cast<unsigned char>(pixelValue);
  }

  /**
   * \brief Returns the value it is passed.
   *
   * \param value A value.
   * \return      The same value.
   */
  template <typename T>
  static T identity(T value)
  {
    return value;
  }
};

}

#endif
