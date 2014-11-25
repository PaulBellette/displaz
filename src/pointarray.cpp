// Copyright (C) 2012, Chris J. Foster and the other authors and contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of the software's owners nor the names of its
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// (This is the BSD 3-clause license)

#include "pointarray.h"

#include "qtlogger.h"

#include "glutil.h"

#include <QtOpenGL/QGLShaderProgram>
#include <QtCore/QTime>

#include <unordered_map>
#include <fstream>

#include "ply_io.h"


//------------------------------------------------------------------------------
/// Functor to compute octree child node index with respect to some given split
/// point
struct OctreeChildIdx
{
    const V3f* m_P;
    V3f m_center;
    int operator()(size_t i)
    {
        V3f p = m_P[i];
        return 4*(p.z >= m_center.z) +
               2*(p.y >= m_center.y) +
                 (p.x >= m_center.x);
    }

    OctreeChildIdx(const V3f* P, const V3f& center) : m_P(P), m_center(center) {}
};


struct OctreeNode
{
    OctreeNode* children[8]; ///< Child nodes - order (x + 2*y + 4*z)
    size_t beginIndex;       ///< Begin index of points in this node
    size_t endIndex;         ///< End index of points in this node
    mutable size_t nextBeginIndex; ///< Next index for incremental rendering
    Imath::Box3f bbox;       ///< Actual bounding box of points in node
    V3f center;              ///< Centre of the node
    float radius;            ///< Distance to centre to node edge along an axis

    OctreeNode(const V3f& center, float radius)
        : beginIndex(0), endIndex(0), nextBeginIndex(0),
        center(center), radius(radius)
    {
        for (int i = 0; i < 8; ++i)
            children[i] = 0;
    }

    ~OctreeNode()
    {
        for (int i = 0; i < 8; ++i)
            delete children[i];
    }

    size_t size() const { return endIndex - beginIndex; }

    bool isLeaf() const { return beginIndex != endIndex; }
};


struct ProgressFunc
{
    PointArray& points;
    size_t totProcessed;

    ProgressFunc(PointArray& points) : points(points), totProcessed(0) {}

    void operator()(size_t additionalProcessed)
    {
        totProcessed += additionalProcessed;
        emit points.loadProgress(int(100*totProcessed/points.pointCount()));
    }
};

/// Create an octree over the given set of points with position P
///
/// The points for consideration in the current node are the set
/// P[inds[beginIndex..endIndex]]; the tree building process sorts the inds
/// array in place so that points for the output leaf nodes are held in
/// the range P[inds[node.beginIndex, node.endIndex)]].  center is the central
/// split point for splitting children of the current node; radius is the
/// current node radius measured along one of the axes.
static OctreeNode* makeTree(int depth, size_t* inds,
                            size_t beginIndex, size_t endIndex,
                            const V3f* P, const V3f& center,
                            float radius, ProgressFunc& progressFunc)
{
    OctreeNode* node = new OctreeNode(center, radius);
    const size_t pointsPerNode = 100000;
    // Limit max depth of tree to prevent infinite recursion when
    // greater than pointsPerNode points lie at the same position in
    // space.  floats effectively have 24 bit of precision in the
    // mantissa, so there's never any point splitting more than 24 times.
    const int maxDepth = 24;
    size_t* beginPtr = inds + beginIndex;
    size_t* endPtr = inds + endIndex;
    if (endIndex - beginIndex <= pointsPerNode || depth >= maxDepth)
    {
        std::random_shuffle(beginPtr, endPtr);
        for (size_t i = beginIndex; i < endIndex; ++i)
            node->bbox.extendBy(P[inds[i]]);
        node->beginIndex = beginIndex;
        node->endIndex = endIndex;
        progressFunc(endIndex - beginIndex);
        return node;
    }
    // Partition points into the 8 child nodes
    size_t* childRanges[9];
    multi_partition(beginPtr, endPtr, OctreeChildIdx(P, center), childRanges+1, 8);
    childRanges[0] = beginPtr;
    // Recursively generate child nodes
    float r = radius/2;
    for (int i = 0; i < 8; ++i)
    {
        size_t childBeginIndex = childRanges[i]   - inds;
        size_t childEndIndex   = childRanges[i+1] - inds;
        if (childEndIndex == childBeginIndex)
            continue;
        V3f c = center + V3f((i     % 2 == 0) ? -r : r,
                             ((i/2) % 2 == 0) ? -r : r,
                             ((i/4) % 2 == 0) ? -r : r);
        node->children[i] = makeTree(depth+1, inds, childBeginIndex,
                                     childEndIndex, P, c, r, progressFunc);
        node->bbox.extendBy(node->children[i]->bbox);
    }
    return node;
}


//------------------------------------------------------------------------------
// PointArray implementation
PointArray::PointArray()
    : m_npoints(0),
    m_positionFieldIdx(-1),
    m_P(0)
{ }


PointArray::~PointArray()
{ }


/// Load point cloud in text format, assuming fields XYZ
bool PointArray::loadText(QString fileName, size_t maxPointCount,
                          std::vector<GeomField>& fields, V3d& offset,
                          size_t& npoints, size_t& totPoints,
                          Imath::Box3d& bbox, V3d& centroid)
{
    V3d Psum(0);
    // Use C file IO here, since it's about 40% faster than C++ streams for
    // large text files (tested on linux x86_64, gcc 4.6.3).
    FILE* inFile = fopen(fileName.toUtf8(), "r");
    if (!inFile)
        return false;
    fseek(inFile, 0, SEEK_END);
    const size_t numBytes = ftell(inFile);
    fseek(inFile, 0, SEEK_SET);
    std::vector<Imath::V3d> points;
    Imath::V3d p;
    size_t readCount = 0;
    // Read three doubles; "%*[^\n]" discards up to just before end of line
    while (fscanf(inFile, " %lf %lf %lf%*[^\n]", &p.x, &p.y, &p.z) == 3)
    {
        points.push_back(p);
        ++readCount;
        if (readCount % 10000 == 0)
            emit loadProgress(int(100*ftell(inFile)/numBytes));
    }
    fclose(inFile);
    totPoints = points.size();
    npoints = points.size();
    // Zero points + nonzero bytes => bad text file
    if (totPoints == 0 && numBytes != 0)
        return false;
    if (totPoints > 0)
        offset = points[0];
    fields.push_back(GeomField(TypeSpec::vec3float32(), "position", npoints));
    V3f* position = (V3f*)fields[0].as<float>();
    for (size_t i = 0; i < npoints; ++i)
    {
        position[i] = points[i] - offset;
        bbox.extendBy(points[i]);
        Psum += points[i];
    }
    if (npoints > 0)
        centroid = (1.0/npoints)*Psum;
    return true;
}


/// Load ascii version of the point cloud library PCD format
bool PointArray::loadPly(QString fileName, size_t maxPointCount,
                         std::vector<GeomField>& fields, V3d& offset,
                         size_t& npoints, size_t& totPoints,
                         Imath::Box3d& bbox, V3d& centroid)
{
    std::unique_ptr<t_ply_, int(*)(p_ply)> ply(
            ply_open(fileName.toUtf8().constData(), NULL, 0, NULL), ply_close);
    if (!ply || !ply_read_header(ply.get()))
        return false;
    // Parse out header data
    p_ply_element vertexElement = findVertexElement(ply.get(), npoints);
    if (vertexElement)
    {
        if (!loadPlyVertexProperties(fileName, ply.get(), vertexElement, fields, offset, npoints))
            return false;
    }
    else
    {
        if (!loadDisplazNativePly(fileName, ply.get(), fields, offset, npoints))
            return false;
    }
    totPoints = npoints;

    // Compute bounding box and centroid
    const V3f* P = (V3f*)fields[0].as<float>();
    V3d Psum(0);
    for (size_t i = 0; i < npoints; ++i)
    {
        Psum += P[i];
        bbox.extendBy(P[i]);
    }
    if (npoints > 0)
        centroid = (1.0/npoints) * Psum;
    centroid += offset;
    bbox.min += offset;
    bbox.max += offset;
    return true;
}


int PointArray::findField(const std::string& name, const TypeSpec& spec)
{
    for (size_t i = 0; i < m_fields.size(); ++i)
    {
        if (m_fields[i].name == name && m_fields[i].spec == spec)
            return i;
    }
    return -1;
}


bool PointArray::loadFile(QString fileName, size_t maxPointCount)
{
    QTime loadTimer;
    loadTimer.start();
    setFileName(fileName);
    // Read file into point data fields.  Use very basic file type detection
    // based on extension.
    size_t totPoints = 0;
    Imath::Box3d bbox;
    V3d offset(0);
    V3d centroid(0);
    emit loadStepStarted("Reading file");
    if (fileName.endsWith(".las") || fileName.endsWith(".laz"))
    {
        if (!loadLas(fileName, maxPointCount, m_fields, offset,
                     m_npoints, totPoints, bbox, centroid))
        {
            return false;
        }
    }
    else if (fileName.endsWith(".ply"))
    {
        if (!loadPly(fileName, maxPointCount, m_fields, offset,
                     m_npoints, totPoints, bbox, centroid))
        {
            return false;
        }
    }
    else
    {
        if (!loadText(fileName, maxPointCount, m_fields, offset,
                      m_npoints, totPoints, bbox, centroid))
        {
            return false;
        }
    }
    // Search for position field
    m_positionFieldIdx = findField("position", TypeSpec::vec3float32());
    if (m_positionFieldIdx == -1)
    {
        g_logger.error("No position field found in file %s", fileName);
        return false;
    }
    m_P = (V3f*)m_fields[m_positionFieldIdx].as<float>();
    setBoundingBox(bbox);
    setOffset(offset);
    setCentroid(centroid);
    emit loadProgress(100);
    g_logger.info("Loaded %d of %d points from file %s in %.2f seconds",
                  m_npoints, totPoints, fileName, loadTimer.elapsed()/1000.0);
    if (totPoints == 0)
    {
        m_rootNode.reset(new OctreeNode(V3f(0), 1));
        return true;
    }

    // Sort points into octree order
    emit loadStepStarted("Sorting points");
    std::unique_ptr<size_t[]> inds(new size_t[m_npoints]);
    for (size_t i = 0; i < m_npoints; ++i)
        inds[i] = i;
    // Expand the bound so that it's cubic.  Not exactly sure it's required
    // here, but cubic nodes sometimes work better the points are better
    // distributed for LoD, splitting is unbiased, etc.
    Imath::Box3f rootBound(bbox.min - offset, bbox.max - offset);
    V3f diag = rootBound.size();
    float rootRadius = std::max(std::max(diag.x, diag.y), diag.z) / 2;
    ProgressFunc progressFunc(*this);
    m_rootNode.reset(makeTree(0, &inds[0], 0, m_npoints, &m_P[0],
                              rootBound.center(), rootRadius, progressFunc));
    // Reorder point fields into octree order
    emit loadStepStarted("Reordering fields");
    for (size_t i = 0; i < m_fields.size(); ++i)
    {
        reorder(m_fields[i], inds.get(), m_npoints);
        emit loadProgress(int(100*(i+1)/m_fields.size()));
    }
    m_P = (V3f*)m_fields[m_positionFieldIdx].as<float>();

    return true;
}


void PointArray::saveFile(QString fileName)
{
    int classificationIdx = findField("classification", TypeSpec::uint8_i());
    if (classificationIdx < 0)
    {
        g_logger.error("No classification attribute in file %s", this->fileName());
        return;
    }

    uint8_t* classification = m_fields[classificationIdx].as<uint8_t>();

    // Hack: save classified points as XYZC
    V3d off = offset();
    std::ofstream out(fileName.toStdString());
    for (size_t i = 0; i < m_npoints; ++i)
    {
        tfm::format(out, "%.3f %.3f %.3f %d\n",
                    m_P[i].x + off.x, m_P[i].y + off.y, m_P[i].z + off.z,
                    classification[i]);
    }
    g_logger.info("Saved classified points to file: %s", fileName);
}


V3d PointArray::pickVertex(const V3d& rayOrigin, const V3d& rayDirection,
                           double longitudinalScale, double* distance) const
{
    size_t idx = closestPointToRay(m_P, m_npoints, rayOrigin - offset(),
                                   rayDirection, longitudinalScale, distance);
    if (m_npoints == 0)
        return V3d(0);
    return V3d(m_P[idx]) + offset();
}


void PointArray::selectVerticesInSphere(const V3d& centre, double radius,
                                        int class1, int class2)
{
    V3f relCenter = centre - offset();
    Box3f sphereBox(relCenter - V3f(radius), relCenter + V3f(radius));

    int classificationIdx = findField("classification", TypeSpec::uint8_i());
    if (classificationIdx < 0)
        return;

    uint8_t* classification = m_fields[classificationIdx].as<uint8_t>();

    double radius2 = radius*radius;

    std::vector<const OctreeNode*> nodeStack;
    nodeStack.push_back(m_rootNode.get());
    while (!nodeStack.empty())
    {
        const OctreeNode* node = nodeStack.back();
        nodeStack.pop_back();
        if (!node->bbox.intersects(sphereBox))
            continue;
        if (!node->isLeaf())
        {
            for (int i = 0; i < 8; ++i)
            {
                OctreeNode* n = node->children[i];
                if (n)
                    nodeStack.push_back(n);
            }
            continue;
        }
        for (size_t i = node->beginIndex; i < node->endIndex; ++i)
        {
            if ((m_P[i] - relCenter).length2() < radius2)
            {
                if (classification[i] == class1)
                    classification[i] = class2;
            }
        }
    }
}


static size_t simplifiedPointCount(const OctreeNode* node, const V3f& cameraPos,
                                   double quality, bool incrementalDraw)
{
    // Distance below which all points will be drawn for a node, at quality == 1
    // TODO: Auto-adjust this for various length scales?
    const double drawAllDist = 100;
    if (node->isLeaf())
    {
        double dist = (node->bbox.center() - cameraPos).length();
        // double diagRadius = node->radius*sqrt(3);
        double diagRadius = node->bbox.size().length()/2;
        // Subtract bucket diagonal dist, since we really want an approx
        // distance to closest point in the bucket, rather than dist to centre.
        dist = std::max(10.0, dist - diagRadius);
        double desiredFraction = std::min(1.0, quality*pow(drawAllDist/dist, 2));
        size_t chunkSize = (size_t)ceil(node->size()*desiredFraction);
        size_t ndraw = chunkSize;
        if (incrementalDraw)
        {
            ndraw = (node->nextBeginIndex >= node->endIndex) ? 0 :
                    std::min(chunkSize, node->endIndex - node->nextBeginIndex);
        }
        return ndraw;
    }
    size_t ndraw = 0;
    for (int i = 0; i < 8; ++i)
    {
        OctreeNode* n = node->children[i];
        if (n)
            ndraw += simplifiedPointCount(n, cameraPos, quality, incrementalDraw);
    }
    return ndraw;
}


size_t PointArray::simplifiedPointCount(const V3d& cameraPos, bool incrementalDraw) const
{
    V3f relCamera = cameraPos - offset();
    return ::simplifiedPointCount(m_rootNode.get(), relCamera, 1.0, incrementalDraw);
}


static void drawTree(const TransformState& transState, const OctreeNode* node)
{
    Imath::Box3f bbox(node->center - Imath::V3f(node->radius),
                        node->center + Imath::V3f(node->radius));
    drawBoundingBox(transState, bbox, Imath::C3f(1));
    drawBoundingBox(transState, node->bbox, Imath::C3f(1,0,0));
    for (int i = 0; i < 8; ++i)
    {
        OctreeNode* n = node->children[i];
        if (n)
            drawTree(transState, n);
    }
}

void PointArray::drawTree(const TransformState& transState) const
{
    ::drawTree(transState, m_rootNode.get());
}


size_t PointArray::drawPoints(QGLShaderProgram& prog, const V3d& cameraPos,
                              double quality, bool incrementalDraw) const
{
    //printActiveShaderAttributes(prog.programId());
    std::vector<ShaderAttribute> activeAttrs = activeShaderAttributes(prog.programId());
    // Figure out shader locations for each point field
    // TODO: attributeLocation() forces the OpenGL usage here to be
    // synchronous.  Does this matter?  (Alternative: bind them ourselves.)
    std::vector<const ShaderAttribute*> attributes;
    for (size_t i = 0; i < m_fields.size(); ++i)
    {
        const GeomField& field = m_fields[i];
        if (field.spec.isArray())
        {
            for (int j = 0; j < field.spec.count; ++j)
            {
                std::string name = tfm::format("%s[%d]", field.name, j);
                attributes.push_back(findAttr(name, activeAttrs));
            }
        }
        else
        {
            attributes.push_back(findAttr(field.name, activeAttrs));
        }
    }
    // Zero out active attributes in case they don't have associated fields
    GLfloat zeros[16] = {0};
    for (size_t i = 0; i < activeAttrs.size(); ++i)
    {
        prog.setAttributeValue((int)i, zeros, activeAttrs[i].rows,
                               activeAttrs[i].cols);
    }
    // Enable attributes which have associated fields
    for (size_t i = 0; i < attributes.size(); ++i)
    {
        if (attributes[i])
            prog.enableAttributeArray(attributes[i]->location);
    }

    // Draw points in each bucket, with total number drawn depending on how far
    // away the bucket is.  Since the points are shuffled, this corresponds to
    // a stochastic simplification of the full point cloud.
    size_t totDrawn = 0;
    V3f relCamera = cameraPos - offset();
    std::vector<const OctreeNode*> nodeStack;
    nodeStack.push_back(m_rootNode.get());
    while (!nodeStack.empty())
    {
        const OctreeNode* node = nodeStack.back();
        nodeStack.pop_back();
        if (!node->isLeaf())
        {
            for (int i = 0; i < 8; ++i)
            {
                OctreeNode* n = node->children[i];
                if (n)
                    nodeStack.push_back(n);
            }
            continue;
        }
        size_t ndraw = node->size();
        double lodMultiplier = 1;
        size_t idx = node->beginIndex;
        if (!incrementalDraw)
            node->nextBeginIndex = node->beginIndex;
        ndraw = ::simplifiedPointCount(node, relCamera, quality, incrementalDraw);
        idx = node->nextBeginIndex;
        if (ndraw == 0)
            continue;
        // For LoD, compute the desired fraction of points for this node.
        //
        // The desired fraction is chosen such that the density of points
        // per solid angle is constant; when removing points, the point
        // radii are scaled up to keep the total area covered by the points
        // constant.
        //lodMultiplier = sqrt(double(node->size())/ndraw);
        for (size_t i = 0, k = 0; i < m_fields.size(); k+=m_fields[i].spec.arraySize(), ++i)
        {
            const GeomField& field = m_fields[i];
            int arraySize = field.spec.arraySize();
            int vecSize = field.spec.vectorSize();
            for (int j = 0; j < arraySize; ++j)
            {
                const ShaderAttribute* attr = attributes[k+j];
                if (!attr)
                    continue;
                char* data = field.data.get() + idx*field.spec.size() +
                             j*field.spec.elsize;
                if (attr->baseType == TypeSpec::Int || attr->baseType == TypeSpec::Uint)
                {
                    glVertexAttribIPointer(attr->location, vecSize,
                                           glBaseType(field.spec), 0, data);
                }
                else
                {
                    glVertexAttribPointer(attr->location, vecSize,
                                          glBaseType(field.spec),
                                          field.spec.fixedPoint, 0, data);
                }
            }
        }
        prog.setUniformValue("pointSizeLodMultiplier", (GLfloat)lodMultiplier);
        glDrawArrays(GL_POINTS, 0, (GLsizei)ndraw);
        node->nextBeginIndex += ndraw;
        totDrawn += ndraw;
    }
    //tfm::printf("Drew %d of total points %d, quality %f\n", totDraw, m_npoints, quality);

    // Disable all attribute arrays - leaving these enabled seems to screw with
    // the OpenGL fixed function pipeline in unusual ways.
    for (size_t i = 0; i < attributes.size(); ++i)
    {
        if (attributes[i])
            prog.disableAttributeArray(attributes[i]->location);
    }
    return totDrawn;
}


