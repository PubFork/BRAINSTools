/*=========================================================================
 *
 *  Copyright SINAPSE: Scalable Informatics for Neuroscience, Processing and Software Engineering
 *            The University of Iowa
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#ifndef __DWIConverter_h
#define __DWIConverter_h
#include <vector>
#include <algorithm>
#include "itkMatrix.h"
#include "itkImageSeriesReader.h"
#include "itkImageFileReader.h"
#include "itkImage.h"
#include "itkDCMTKFileReader.h"
#include "itkDCMTKImageIO.h"
#include "StringContains.h"
#include <algorithm>
#include "DWIMetaDataDictionaryValidator.h"

/** the DWIConverter is a base class for all scanner-specific
 *  converters.  It handles the tasks that are required for all
 *  scanners. In particular it loads the DICOM directory, and fills
 *  out various data fields needed by the DWIConvert program in order
 *  to write out NRRD and other files.
 */
class DWIConverter
{
public:
  typedef short                               PixelValueType;
  typedef itk::Image<PixelValueType, 3>       VolumeType;
  typedef VolumeType::SpacingType             SpacingType;
  typedef itk::ImageSeriesReader<VolumeType>  ReaderType;
  typedef ReaderType::FileNamesContainer      FileNamesContainer;
  typedef itk::ImageFileReader<VolumeType>    SingleFileReaderType;
  typedef itk::DCMTKSeriesFileNames           InputNamesGeneratorType;
  typedef std::vector<itk::DCMTKFileReader *> DCMTKFileVector;
  typedef itk::Matrix<double, 3, 3>           RotationMatrixType;
  typedef itk::Vector<double, 3>              PointType;
  DWIConverter(const DCMTKFileVector &allHeaders,
               const FileNamesContainer &inputFileNames,
               const bool useBMatrixGradientDirections) : m_Headers(allHeaders),
                                                    m_InputFileNames(inputFileNames),
                                                    m_MultiSliceVolume(false),
                                                    m_SliceOrderIS(true),
                                                    m_Rows(0),
                                                    m_Cols(0),
                                                    m_SlicesPerVolume(0),
                                                    m_NSlice(0),
                                                    m_NVolume(0),
                                                    m_UseBMatrixGradientDirections(useBMatrixGradientDirections),
                                                    m_useIdentityMeaseurementFrame(false),
                                                    m_IsInterleaved(false)
    {

      this->m_NRRDSpaceDefinition = "left-posterior-superior";;
      this->m_MeasurementFrame.SetIdentity();
    }

  virtual ~DWIConverter() {}

  virtual void LoadDicomDirectory()
    {
      //
      // add vendor-specific flags to dictionary
      this->AddFlagsToDictionary();
      //
      // load the volume, either single or multivolume.
      m_NSlice = this->m_InputFileNames.size();
      itk::DCMTKImageIO::Pointer dcmtkIO = itk::DCMTKImageIO::New();
      if( this->m_InputFileNames.size() > 1 )
        {
        ReaderType::Pointer reader = ReaderType::New();
        reader->SetImageIO( dcmtkIO );
        reader->SetFileNames( this->m_InputFileNames );
        try
          {
          reader->Update();
          }
        catch( itk::ExceptionObject & excp )
          {
          std::cerr << "Exception thrown while reading DICOM volume"
                    << std::endl;
          std::cerr << excp << std::endl;
          throw;
          }
        m_Volume = reader->GetOutput();
        m_MultiSliceVolume = false;
        }
      else
        {
        SingleFileReaderType::Pointer reader =
          SingleFileReaderType::New();
        reader->SetImageIO( dcmtkIO );
        reader->SetFileName( this->m_InputFileNames[0] );
        m_NSlice = this->m_InputFileNames.size();
        try
          {
          reader->Update();
          }
        catch( itk::ExceptionObject & excp )
          {
          std::cerr << "Exception thrown while reading the series" << std::endl;
          std::cerr << excp << std::endl;
          throw;
          }
        m_Volume = reader->GetOutput();
        m_MultiSliceVolume = true;
        }

      // figure out image dimensions
      m_Headers[0]->GetElementUS(0x0028, 0x0010, this->m_Rows);
      m_Headers[0]->GetElementUS(0x0028, 0x0011, this->m_Cols);

      {
      // origin
      double origin[3];
      m_Headers[0]->GetOrigin(origin);
      VolumeType::PointType imOrigin;
      imOrigin[0] = origin[0];
      imOrigin[1] = origin[1];
      imOrigin[2] = origin[2];
        this->m_Volume->SetOrigin(imOrigin);
      }
      // spacing
      {

      double spacing[3];
      m_Headers[0]->GetSpacing(spacing);
      SpacingType imSpacing;
      imSpacing[0] = spacing[0];
      imSpacing[1] = spacing[1];
      imSpacing[2] = spacing[2];
      m_Volume->SetSpacing(imSpacing);
      }

      // a map of ints keyed by the slice location string
      // reported in the dicom file.  The number of slices per
      // volume is the same as the number of unique slice locations
      std::map<std::string, int> sliceLocations;
      //
      // check for interleave
      if( !this->m_MultiSliceVolume )
        {
        // Make a hash of the sliceLocations in order to get the correct
        // count.  This is more reliable since SliceLocation may not be available.
        std::vector<int>         sliceLocationIndicator;
        std::vector<std::string> sliceLocationStrings;

        sliceLocationIndicator.resize( this->m_NSlice );
        for( unsigned int k = 0; k < this->m_NSlice; ++k )
          {
          std::string originString;
          this->m_Headers[k]->GetElementDS(0x0020, 0x0032, originString );
          sliceLocationStrings.push_back( originString );
          sliceLocations[originString]++;
          }

        // this seems like a crazy way to figure out if slices are
        // interleaved, but it works. Perhaps replace with comparing
        // the reported location between the first two slices?
        // Would be less clever-looking and devious, but would require
        // less computation.
        for( unsigned int k = 0; k < this->m_NSlice; ++k )
          {
          std::map<std::string, int>::iterator it = sliceLocations.find( sliceLocationStrings[k] );
          sliceLocationIndicator[k] = distance( sliceLocations.begin(), it );
          }

        // sanity check on # of volumes versus # of dicom files
        if(this->m_Headers.size() % sliceLocations.size() != 0)
          {
          itkGenericExceptionMacro(<< "Missing DICOM Slice files: Number of slice files ("
                            << this->m_Headers.size() << ") not evenly divisible by"
                            << " the number of slice locations ");
          }

        this->m_SlicesPerVolume = sliceLocations.size();
        std::cout << "=================== this->m_SlicesPerVolume:" << this->m_SlicesPerVolume << std::endl;


        // if the this->m_SlicesPerVolume == 1, de-interleaving won't do
        // anything so there's no point in doing it.
        if( this->m_NSlice >= 2 && this->m_SlicesPerVolume > 1 )
          {
          if( sliceLocationIndicator[0] != sliceLocationIndicator[1] )
            {
            std::cout << "Dicom images are ordered in a volume interleaving way." << std::endl;
            }
          else
            {
            std::cout << "Dicom images are ordered in a slice interleaving way." << std::endl;
            this->m_IsInterleaved = true;
            // reorder slices into a volume interleaving manner
            DeInterleaveVolume();
            }
          }
        }

    {
    VolumeType::DirectionType LPSDirCos;
    LPSDirCos.SetIdentity();

    // check ImageOrientationPatient and figure out slice direction in
    // L-P-I (right-handed) system.
    // In Dicom, the coordinate frame is L-P by default. Look at
    // http://medical.nema.org/dicom/2007/07_03pu.pdf ,  page 301
    double dirCosArray[6];
    // 0020,0037 -- Image Orientation (Patient)
    this->m_Headers[0]->GetDirCosArray(dirCosArray);
    double *dirCosArrayP = dirCosArray;
    for( unsigned i = 0; i < 2; ++i )
      {
      for( unsigned j = 0; j < 3; ++j, ++dirCosArrayP )
        {
        LPSDirCos[j][i] = *dirCosArrayP;
        }
      }

    // Cross product, this gives I-axis direction
    LPSDirCos[0][2] = LPSDirCos[1][0] * LPSDirCos[2][1] - LPSDirCos[2][0] * LPSDirCos[1][1];
    LPSDirCos[1][2] = LPSDirCos[2][0] * LPSDirCos[0][1] - LPSDirCos[0][0] * LPSDirCos[2][1];
    LPSDirCos[2][2] = LPSDirCos[0][0] * LPSDirCos[1][1] - LPSDirCos[1][0] * LPSDirCos[0][1];

    this->m_Volume->SetDirection(LPSDirCos);
    }
    std::cout << "ImageOrientationPatient (0020:0037): ";
    std::cout << "LPS Orientation Matrix" << std::endl;
    std::cout << this->m_Volume->GetDirection() << std::endl;


    std::cout << "this->m_SpacingMatrix" << std::endl;
    std::cout << this->GetSpacingMatrix() << std::endl;

    std::cout << "NRRDSpaceDirection" << std::endl;
    std::cout << this->GetNRRDSpaceDirection() << std::endl;

    }

  RotationMatrixType GetSpacingMatrix() const
  {
    RotationMatrixType SpacingMatrix;
    SpacingMatrix.Fill(0.0);
    SpacingMatrix[0][0] = this->m_Volume->GetSpacing()[0];
    SpacingMatrix[1][1] = this->m_Volume->GetSpacing()[1];
    SpacingMatrix[2][2] = this->m_Volume->GetSpacing()[2];
    return SpacingMatrix;
  }
  /** extract dwi data -- vendor specific so must happen in subclass
   *  implementing this method.
   */
  virtual void ExtractDWIData() = 0;

  /** access methods for image data */
  const DWIMetaDataDictionaryValidator::GradientTableType &GetDiffusionVectors() const { return this->m_DiffusionVectors; }

  const DWIMetaDataDictionaryValidator::GradientTableType computeScaledDiffusionVectors() const
  {
    //TODO: This can be further simplified.  Move private computeScaled(...) into this logic
    const DWIMetaDataDictionaryValidator::GradientTableType& UnitNormDiffusionVectors = this->GetDiffusionVectors();
    const std::vector<double>& bValues = this->GetBValues();
    const double maxBvalue = this->GetMaxBValue();
    const DWIMetaDataDictionaryValidator::GradientTableType BvalueScaledDiffusionVectors =
      this->computeScaledDiffusionVectors(UnitNormDiffusionVectors, bValues, maxBvalue);
    return BvalueScaledDiffusionVectors;
  }

  const std::vector<double> &GetBValues() const { return this->m_BValues; }
  double GetMaxBValue() const { return ComputeMaxBvalue( this->m_BValues); }
  void SetMeasurementFrameIdentity()
  {
    this->m_MeasurementFrame.SetIdentity();
  }

  VolumeType::Pointer GetDiffusionVolume() const { return this->m_Volume; }

  SpacingType GetSpacing() const
    {
      return this->m_Volume->GetSpacing();
    }

  VolumeType::PointType GetOrigin() const
    {
      return this->m_Volume->GetOrigin();
    }

  RotationMatrixType   GetLPSDirCos() const { return this->m_Volume->GetDirection(); }

  RotationMatrixType GetMeasurementFrame() const { return this->m_MeasurementFrame; }

  RotationMatrixType GetNRRDSpaceDirection() const { return  this->m_Volume->GetDirection() * this->GetSpacingMatrix(); }

  unsigned int GetNVolume() const { return this->m_NVolume; }

  std::string GetNRRDSpaceDefinition() const { return this->m_NRRDSpaceDefinition; }

  unsigned short GetRows() const { return m_Rows; }

  unsigned short GetCols() const { return m_Cols; }

  unsigned int GetSlicesPerVolume() const { return m_SlicesPerVolume; }



  /**
   * @brief Force overwriting the gradient directions by inserting values read from specified file
   * @param gradientVectorFile The file with gradients specified for overwriting
   * FORMAT:
   * <num_gradients>
   * x y z
   * x y z
   * etc
   */
  void readOverwriteGradientVectorFile(const std::string& gradientVectorFile)
  {// override gradients embedded in file with an external file.
    std::__1::ifstream gradientFile(gradientVectorFile.c_str(), std::__1::ios_base::in);
    unsigned int  numGradients;
    gradientFile >> numGradients;
    if( numGradients != this->GetNVolume() )
    {
      itkGenericExceptionMacro( << "number of Gradients doesn't match number of volumes" << std::endl);
    }
    this->m_DiffusionVectors.clear();
    for( unsigned int imageCount = 0; !gradientFile.eof(); ++imageCount )
    {
      DWIMetaDataDictionaryValidator::GradientDirectionType vec;
      for( unsigned i = 0; !gradientFile.eof() &&  i < 3; ++i )
      {
        gradientFile >> vec[i];
      }
      this->m_DiffusionVectors.push_back(vec);
    }
  }


  DWIMetaDataDictionaryValidator::GradientTableType
  computeBvalueScaledDiffusionTensors() const
  {

    const DWIMetaDataDictionaryValidator::GradientTableType &BvalueScaledDiffusionVectors =
      this->computeScaledDiffusionVectors();
    DWIMetaDataDictionaryValidator::GradientTableType gradientVectors;
    const vnl_matrix_fixed<double, 3, 3> InverseMeasurementFrame = this->GetMeasurementFrame().GetInverse();
    // grab the diffusion vectors.
    for( unsigned int k = 0; k < BvalueScaledDiffusionVectors.size(); ++k )
    {
      DWIMetaDataDictionaryValidator::GradientDirectionType vec;
      if( this->m_useIdentityMeaseurementFrame )
      {
        // For scanners, the measurement frame for the gradient directions is the same as the
        // Excerpt from http://teem.sourceforge.net/nrrd/format.html definition of "measurement frame:"
        // There is also the possibility that a measurement frame
        // should be recorded for an image even though it is storing
        // only scalar values (e.g., a sequence of diffusion-weighted MR
        // images has a measurement frame for the coefficients of
        // the diffusion-sensitizing gradient directions, and
        // the measurement frame field is the logical store
        // this information).
        // It was noticed on oblique Philips DTI scans that the prescribed protocol directions were
        // rotated by the ImageOrientationPatient amount and recorded in the DICOM header.
        // In order to compare two different scans to determine if the same protocol was prosribed,
        // it is necessary to multiply each of the recorded diffusion gradient directions by
        // the inverse of the LPSDirCos.
        vnl_vector_fixed<double,3> RotatedScaledDiffusionVectors = InverseMeasurementFrame * (BvalueScaledDiffusionVectors[k]);
        for( unsigned ind = 0; ind < 3; ++ind )
        {
          vec[ind] = RotatedScaledDiffusionVectors[ind];
        }
      }
      else
      {
        for( unsigned ind = 0; ind < 3; ++ind )
        {
          vec[ind] = BvalueScaledDiffusionVectors[k][ind];
        }
      }
      gradientVectors.push_back(vec);
    }
    return gradientVectors;
  }


  std::string
  MakeFileComment(
    const std::string& version,
    bool useBMatrixGradientDirections,
    bool useIdentityMeaseurementFrame,
    double smallGradientThreshold ) const
  {
    std::__1::stringstream commentSection;
    {
      commentSection << "#" << std::__1::endl << "#" << std::__1::endl;
      commentSection << "# This file was created by DWIConvert version " << version << std::__1::endl
                     << "# https://github.com/BRAINSia/BRAINSTools" << std::__1::endl
                     << "# part of the BRAINSTools package." << std::__1::endl
                     << "# Command line options:" << std::__1::endl
                     << "# --smallGradientThreshold " << smallGradientThreshold << std::__1::endl;
      if (useIdentityMeaseurementFrame) {
        commentSection << "# --useIdentityMeasurementFrame" << std::__1::endl;
      }
      if (useBMatrixGradientDirections) {
        commentSection << "# --useBMatrixGradientDirections" << std::__1::endl;
      }
    }
    return commentSection.str();
  }

  void ManualWriteNRRDFile(
    const std::string& outputVolumeHeaderName,
    const std::string commentstring) const
  {
    const size_t extensionPos = outputVolumeHeaderName.find(".nhdr");
    const bool nrrdSingleFileFormat = ( extensionPos != std::string::npos ) ? false : true;

    std::string outputVolumeDataName;
    if( extensionPos != std::string::npos )
    {
      outputVolumeDataName = outputVolumeHeaderName.substr(0, extensionPos);
      outputVolumeDataName += ".raw";
    }

    itk::NumberToString<double> DoubleConvert;
    std::__1::ofstream header;
    // std::string headerFileName = outputDir + "/" + outputFileName;

    const double maxBvalue = this->GetMaxBValue();
    header.open(outputVolumeHeaderName.c_str(), std::__1::ios_base::out | std::__1::ios_base::binary);
    header << "NRRD0005" << std::__1::endl
           << std::__1::setprecision(17) << std::__1::scientific;

    header << commentstring;

    // stamp with DWIConvert branding

    if (!nrrdSingleFileFormat) {
      header << "content: exists(" << itksys::SystemTools::GetFilenameName(outputVolumeDataName) << ",0)"
             << std::__1::endl;
    }
    header << "type: short" << std::__1::endl;
    header << "dimension: 4" << std::__1::endl;
    header << "space: " << this->GetNRRDSpaceDefinition() << "" << std::__1::endl;

    const DWIConverter::RotationMatrixType& NRRDSpaceDirection = this->GetNRRDSpaceDirection();
    header << "sizes: " << this->GetCols()
           << " " << this->GetRows()
           << " " << this->GetSlicesPerVolume()
           << " " << this->GetNVolume() << std::__1::endl;
    header << "thicknesses:  NaN  NaN " << DoubleConvert(this->GetSpacing()[2]) << " NaN" << std::__1::endl;
    // need to check
    header << "space directions: "
           << "("
           << DoubleConvert(NRRDSpaceDirection[0][0]) << ","
           << DoubleConvert(NRRDSpaceDirection[1][0]) << ","
           << DoubleConvert(NRRDSpaceDirection[2][0])
           << ") "
           << "("
           << DoubleConvert(NRRDSpaceDirection[0][1]) << ","
           << DoubleConvert(NRRDSpaceDirection[1][1]) << ","
           << DoubleConvert(NRRDSpaceDirection[2][1]) << ") "
           << "("
           << DoubleConvert(NRRDSpaceDirection[0][2]) << ","
           << DoubleConvert(NRRDSpaceDirection[1][2]) << ","
           << DoubleConvert(NRRDSpaceDirection[2][2])
           << ") none" << std::__1::endl;
    header << "centerings: cell cell cell ???" << std::__1::endl;
    header << "kinds: space space space list" << std::__1::endl;

    header << "endian: little" << std::__1::endl;
    header << "encoding: raw" << std::__1::endl;
    header << "space units: \"mm\" \"mm\" \"mm\"" << std::__1::endl;

    const DWIConverter::VolumeType::PointType ImageOrigin = this->GetOrigin();
    header << "space origin: "
           << "(" << DoubleConvert(ImageOrigin[0])
           << "," << DoubleConvert(ImageOrigin[1])
           << "," << DoubleConvert(ImageOrigin[2]) << ") " << std::__1::endl;
    if (!nrrdSingleFileFormat) {
      header << "data file: " << itksys::SystemTools::GetFilenameName(outputVolumeDataName) << std::__1::endl;
    }

    DWIConverter::RotationMatrixType MeasurementFrame = this->GetMeasurementFrame();
    if (this->m_useIdentityMeaseurementFrame) {
      MeasurementFrame.SetIdentity();
    }
    {
      header << "measurement frame: "
             << "(" << DoubleConvert(MeasurementFrame[0][0]) << ","
             << DoubleConvert(MeasurementFrame[1][0]) << ","
             << DoubleConvert(MeasurementFrame[2][0]) << ") "
             << "(" << DoubleConvert(MeasurementFrame[0][1]) << ","
             << DoubleConvert(MeasurementFrame[1][1]) << ","
             << DoubleConvert(MeasurementFrame[2][1]) << ") "
             << "(" << DoubleConvert(MeasurementFrame[0][2]) << ","
             << DoubleConvert(MeasurementFrame[1][2]) << ","
             << DoubleConvert(MeasurementFrame[2][2]) << ")"
             << std::__1::endl;
    }

    header << "modality:=DWMRI" << std::__1::endl;
    // this is the norminal BValue, i.e. the largest one.
    header << "DWMRI_b-value:=" << DoubleConvert(maxBvalue) << std::__1::endl;

    //  the following three lines are for older NRRD format, where
    //  baseline images are always in the begining.
    //  header << "DWMRI_gradient_0000:=0  0  0" << std::endl;
    //  header << "DWMRI_NEX_0000:=" << nBaseline << std::endl;
    //  need to check
    const DWIMetaDataDictionaryValidator::GradientTableType & gradientVectors =
      this->computeBvalueScaledDiffusionTensors();
    {
      unsigned int gradientVecIndex = 0;
      for (unsigned int k = 0; k<gradientVectors.size(); ++k) {
        header << "DWMRI_gradient_" << std::__1::setw(4) << std::__1::setfill('0') << k << ":="
               << DoubleConvert(gradientVectors[gradientVecIndex][0]) << "   "
               << DoubleConvert(gradientVectors[gradientVecIndex][1]) << "   "
               << DoubleConvert(gradientVectors[gradientVecIndex][2])
               << std::__1::endl;
        ++gradientVecIndex;
      }
    }
    // write data in the same file is .nrrd was chosen
    header << std::__1::endl;;
    if (nrrdSingleFileFormat) {
      unsigned long nVoxels = this->GetDiffusionVolume()->GetBufferedRegion().GetNumberOfPixels();
      header.write(reinterpret_cast<char*>(this->GetDiffusionVolume()->GetBufferPointer()),
        nVoxels*sizeof(short));
    }
    else {
      // if we're writing out NRRD, and the split header/data NRRD
      // format is used, write out the image as a raw volume.
      itk::ImageFileWriter<DWIConverter::VolumeType>::Pointer
        rawWriter = itk::ImageFileWriter<DWIConverter::VolumeType>::New();
      itk::RawImageIO<DWIConverter::PixelValueType,3>::Pointer rawIO
        = itk::RawImageIO<DWIConverter::PixelValueType,3>::New();
      rawWriter->SetImageIO(rawIO);
      rawIO->SetByteOrderToLittleEndian();
      rawWriter->SetFileName(outputVolumeDataName.c_str());
      rawWriter->SetInput(this->GetDiffusionVolume());
      try {
        rawWriter->Update();
      }
      catch (itk::ExceptionObject& excp) {
        std::__1::cerr << "Exception thrown while writing the series to"
                       << outputVolumeDataName << " " << excp << std::__1::endl;
        std::__1::cerr << excp << std::__1::endl;
      }
    }
    header.close();
  }

  void SetUseIdentityMeaseurementFrame(const bool value)
  {
    this->m_useIdentityMeaseurementFrame = value;
  }

/** the DICOM datasets are read as 3D volumes, but they need to be
 *  written as 4D volumes for image types other than NRRD.
 */
  int
  WriteFSLFormattedFileSet(const std::string& outputVolumeHeaderName,
    const std::string outputBValues, const std::string
  outputBVectors) const
  {
    DWIConverter::VolumeType::Pointer img = this->GetDiffusionVolume();
    const int nVolumes = this->GetNVolume();
    typedef itk::Image<DWIConverter::PixelValueType, 4> Volume4DType;

    DWIConverter::VolumeType::SizeType      size3D(img->GetLargestPossibleRegion().GetSize() );
    DWIConverter::VolumeType::DirectionType direction3D(img->GetDirection() );
    DWIConverter::VolumeType::SpacingType   spacing3D(img->GetSpacing() );
    DWIConverter::VolumeType::PointType     origin3D(img->GetOrigin() );

    Volume4DType::SizeType size4D;
    size4D[0] = size3D[0];
    size4D[1] = size3D[1];
    size4D[2] = size3D[2] / nVolumes;
    size4D[3] = nVolumes;

    if( (size4D[2] * nVolumes) != size3D[2] )
    {
      std::cerr << "#of slices in volume not evenly divisible by"
                << " the number of volumes: slices = " << size3D[2]
                << " volumes = " << nVolumes << " left-over slices = "
                << size3D[2] % nVolumes << std::endl;
    }
    Volume4DType::DirectionType direction4D;
    Volume4DType::SpacingType   spacing4D;
    Volume4DType::PointType     origin4D;
    for( unsigned i = 0; i < 3; ++i )
    {
      for( unsigned j = 0; j < 3; ++j )
      {
        direction4D[i][j] = direction3D[i][j];
      }
      direction4D[3][i] = 0.0;
      direction4D[i][3] = 0.0;
      spacing4D[i] = spacing3D[i];
      origin4D[i] = origin3D[i];
    }
    direction4D[3][3] = 1.0;
    spacing4D[3] = 1.0;
    origin4D[3] = 0.0;

    Volume4DType::Pointer img4D = Volume4DType::New();
    img4D->SetRegions(size4D);
    img4D->SetDirection(direction4D);
    img4D->SetSpacing(spacing4D);
    img4D->SetOrigin(origin4D);

    img4D->Allocate();
    img4D->SetMetaDataDictionary(img->GetMetaDataDictionary());
    size_t bytecount = img4D->GetLargestPossibleRegion().GetNumberOfPixels();
    bytecount *= sizeof(DWIConverter::PixelValueType);
    memcpy(img4D->GetBufferPointer(), img->GetBufferPointer(), bytecount);
//#define DEBUG_WRITE4DVOLUME
#ifdef DEBUG_WRITE4DVOLUME
    {
   {
      //Set the qform and sfrom codes for the MetaDataDictionary.
      itk::MetaDataDictionary & thisDic = img->GetMetaDataDictionary();
      itk::EncapsulateMetaData< std::string >( thisDic, "qform_code_name", "NIFTI_XFORM_SCANNER_ANAT" );
      itk::EncapsulateMetaData< std::string >( thisDic, "sform_code_name", "NIFTI_XFORM_UNKNOWN" );
   }
    itk::ImageFileWriter<DWIConverter::VolumeType>::Pointer writer = itk::ImageFileWriter<DWIConverter::VolumeType>::New();
    writer->SetFileName( "/tmp/dwi3dconvert.nii.gz");
    writer->SetInput( img );
    writer->Update();
    }
#endif

    {
      //Set the qform and sfrom codes for the MetaDataDictionary.
      itk::MetaDataDictionary & thisDic = img4D->GetMetaDataDictionary();
      itk::EncapsulateMetaData< std::string >( thisDic, "qform_code_name", "NIFTI_XFORM_SCANNER_ANAT" );
      itk::EncapsulateMetaData< std::string >( thisDic, "sform_code_name", "NIFTI_XFORM_UNKNOWN" );
    }
    itk::ImageFileWriter<Volume4DType>::Pointer imgWriter = itk::ImageFileWriter<Volume4DType>::New();

    imgWriter->SetInput( img4D );
    imgWriter->SetFileName( outputVolumeHeaderName.c_str() );
    try
    {
      imgWriter->Update();
    }
    catch( itk::ExceptionObject & excp )
    {
      std::cerr << "Exception thrown while writing "
                << outputVolumeHeaderName << std::endl;
      std::cerr << excp << std::endl;
      return EXIT_FAILURE;
    }
    // FSL output of gradients & BValues
    std::string outputFSLBValFilename;
    const size_t extensionPos = this->has_valid_nifti_extension(outputVolumeHeaderName);
    if( outputBValues == "" )
    {
      outputFSLBValFilename = outputVolumeHeaderName.substr(0, extensionPos);
      outputFSLBValFilename += ".bval";
    }
    else
    {
      outputFSLBValFilename = outputBValues;
    }
    std::string outputFSLBVecFilename;
    if( outputBVectors == "" )
    {
      outputFSLBVecFilename = outputVolumeHeaderName.substr(0, extensionPos);
      outputFSLBVecFilename += ".bvec";
    }
    else
    {
      outputFSLBVecFilename = outputBVectors;
    }
    // write out in FSL format
    if( WriteBValues<double>(this->GetBValues(), outputFSLBValFilename) != EXIT_SUCCESS )
    {
      std::cerr << "Failed to write " << outputFSLBValFilename
                << std::endl;
      return EXIT_FAILURE;
    }
    if( WriteBVectors(this->computeBvalueScaledDiffusionTensors(), outputFSLBVecFilename) != EXIT_SUCCESS )
    {
      std::cerr << "Failed to write " << outputFSLBVecFilename
                << std::endl;
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

protected:
  /* determine if slice order is inferior to superior */
  void DetermineSliceOrderIS()
    {
      double image0Origin[3];
      image0Origin[0]=this->m_Volume->GetOrigin()[0];
      image0Origin[1]=this->m_Volume->GetOrigin()[1];
      image0Origin[2]=this->m_Volume->GetOrigin()[2];
      std::cout << "Slice 0: " << image0Origin[0] << " "
                << image0Origin[1] << " " << image0Origin[2] << std::endl;

      // assume volume interleaving, i.e. the second dicom file stores
      // the second slice in the same volume as the first dicom file
      double image1Origin[3];

      unsigned long nextSlice = 0;
      if (this->m_Headers.size() > 1)
        {
        // assuming multiple files is invalid for single-file volume: http://www.na-mic.org/Bug/view.php?id=4105
        nextSlice = this->m_IsInterleaved ? this->m_NVolume : 1;
        }

      this->m_Headers[nextSlice]->GetOrigin(image1Origin);
      std::cout << "Slice " << nextSlice << ": " << image1Origin[0] << " " << image1Origin[1]
                << " " << image1Origin[2] << std::endl;

      image1Origin[0] -= image0Origin[0];
      image1Origin[1] -= image0Origin[1];
      image1Origin[2] -= image0Origin[2];
      const DWIConverter::RotationMatrixType & NRRDSpaceDirection = this->GetNRRDSpaceDirection();
      double x1 = image1Origin[0] * (NRRDSpaceDirection[0][2])
        + image1Origin[1] * (NRRDSpaceDirection[1][2])
        + image1Origin[2] * (NRRDSpaceDirection[2][2]);
      if( x1 < 0 )
        {
        this->m_SliceOrderIS = false;
        }
    }
  /** the SliceOrderIS flag can be computed (as above) but if it's
   *  invariant, the derived classes can just set the flag. This method
   *  fixes up the VolumeDirectionCos after the flag is set.
   */
  void SetDirectionsFromSliceOrder()
    {
      if(this->m_SliceOrderIS)
        {
        std::cout << "Slice order is IS" << std::endl;
        }
      else
      {
        std::cout << "Slice order is SI" << std::endl;
        VolumeType::DirectionType LPSDirCos = this->m_Volume->GetDirection();
        LPSDirCos[0][2] = -LPSDirCos[0][2];
        LPSDirCos[1][2] = -LPSDirCos[1][2];
        LPSDirCos[2][2] = -LPSDirCos[2][2];
        this->m_Volume->SetDirection(LPSDirCos);
      }
    }

  /* given a sequence of dicom files where all the slices for location
   * 0 are folled by all the slices for location 1, etc. This method
   * transforms it into a sequence of volumes
   */
  void
  DeInterleaveVolume()
    {
      size_t NVolumes = this->m_NSlice / this->m_SlicesPerVolume;

      VolumeType::RegionType R = this->m_Volume->GetLargestPossibleRegion();

      R.SetSize(2, 1);
      std::vector<VolumeType::PixelType> v(this->m_NSlice);
      std::vector<VolumeType::PixelType> w(this->m_NSlice);

      itk::ImageRegionIteratorWithIndex<VolumeType> I(this->m_Volume, R );
      // permute the slices by extracting the 1D array of voxels for
      // a particular {x,y} position, then re-ordering the voxels such
      // that all the voxels for a particular volume are adjacent
      for( I.GoToBegin(); !I.IsAtEnd(); ++I )
        {
        VolumeType::IndexType idx = I.GetIndex();
        // extract all values in one "column"
        for( unsigned int k = 0; k < this->m_NSlice; ++k )
          {
          idx[2] = k;
          v[k] = this->m_Volume->GetPixel( idx );
          }
        // permute
        for( unsigned int k = 0; k < NVolumes; ++k )
          {
          for( unsigned int m = 0; m < this->m_SlicesPerVolume; ++m )
            {
            w[(k * this->m_SlicesPerVolume) + m] = v[(m * NVolumes) + k];
            }
          }
        // put things back in order
        for( unsigned int k = 0; k < this->m_NSlice; ++k )
          {
          idx[2] = k;
          this->m_Volume->SetPixel( idx, w[k] );
          }
        }
    }

  double ComputeMaxBvalue(const std::vector<double> &bValues) const
  {
    double maxBvalue(0.0);
    for( unsigned int k = 0; k < bValues.size(); ++k )
    {
      if( bValues[k] > maxBvalue )
      {
        maxBvalue = bValues[k];
      }
    }
    return maxBvalue;
  }

  size_t has_valid_nifti_extension( std::string outputVolumeHeaderName ) const
  {
    const size_t NUMEXT=2;
    const char * const extList [NUMEXT] = {".nii.gz", ".nii"};
    for(size_t i = 0 ; i < NUMEXT; ++i)
    {
      const size_t extensionPos = outputVolumeHeaderName.find(extList[i]);
      if( extensionPos != std::string::npos )
      {
        return extensionPos;
      }
    }
    {
      std::cerr << "FSL Format output chosen, "
                << "but output Volume not a recognized "
                << "NIfTI filename " << outputVolumeHeaderName
                << std::endl;
      exit(1);
    }
    return std::string::npos;
  }

  DWIMetaDataDictionaryValidator::GradientTableType
  computeScaledDiffusionVectors( const DWIMetaDataDictionaryValidator::GradientTableType &UnitNormDiffusionVectors,
    const std::vector<double> &bValues,
    const double maxBvalue) const
  {
    DWIMetaDataDictionaryValidator::GradientTableType BvalueScaledDiffusionVectors;
    for( unsigned int k = 0; k < UnitNormDiffusionVectors.size(); ++k )
    {
      vnl_vector_fixed<double,3> vec(3);
      float scaleFactor = 0;
      if( maxBvalue > 0 )
      {
        scaleFactor = sqrt( bValues[k] / maxBvalue );
      }
      std::cout << "Scale Factor for Multiple BValues: " << k << " -- sqrt( " << bValues[k] << " / " << maxBvalue << " ) = "
                << scaleFactor << std::endl;
      for( unsigned ind = 0; ind < 3; ++ind )
      {
        vec[ind] = UnitNormDiffusionVectors[k][ind] * scaleFactor;
      }
      BvalueScaledDiffusionVectors.push_back(vec);
    }
    return BvalueScaledDiffusionVectors;
  }

  /** add vendor-specific flags; */
  virtual void AddFlagsToDictionary() = 0;
  /** one file reader per DICOM file in dataset */
  const DCMTKFileVector     m_Headers;
  /** the names of all the filenames, needed to use
   *  itk::ImageSeriesReader
   */
  const FileNamesContainer  m_InputFileNames;


  /** measurement from for gradients if different than patient
   *  reference frame.
   */
  RotationMatrixType   m_MeasurementFrame;
  /** potentially the measurement frame */
  /** matrix with just spacing information, used a couple places */
  /** the current dataset is represented in a single file */
  bool                m_MultiSliceVolume;
  /** slice order is inferior/superior? */
  bool                m_SliceOrderIS;
  /** the image read from the DICOM dataset */
  VolumeType::Pointer m_Volume;

  /** dimensions */
  unsigned short      m_Rows;
  unsigned short      m_Cols;
  unsigned int        m_SlicesPerVolume;
  /** number of total slices */
  unsigned int        m_NSlice;
  /** number of gradient volumes */
  unsigned int        m_NVolume;


  /** list of B Values for each volume */
  std::vector<double>  m_BValues;
  /** list of gradient vectors */
  DWIMetaDataDictionaryValidator::GradientTableType  m_DiffusionVectors;
  /** double conversion instance, for optimal printing of numbers as
   *  text
   */
  itk::NumberToString<double> m_DoubleConvert;
  /** use the BMatrix to compute gradients in Siemens data instead of
   *  the reported graients. which are in many cases bogus.
   */
  const bool m_UseBMatrixGradientDirections;
  bool       m_useIdentityMeaseurementFrame;

  /** track if images is interleaved */
  bool                        m_IsInterleaved;
  /** again this is always the same (so far) but someone thought
   *  it might be important to change it.
   */
  std::string                 m_NRRDSpaceDefinition;
};

#endif // __DWIConverter_h
