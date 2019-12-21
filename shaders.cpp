#include "shaders.hpp"

#include <set>
#include <iomanip>
#include <iostream>
#include <QApplication>
#include <QFile>
#include <QDir>

#include "data.hpp"
#include "util.hpp"

#include "config.h"

QString withHeadersIncluded(QString src, QString const& filename);

void initConstHeader()
{
    constantsHeader="const float earthRadius="+QString::number(earthRadius)+"; // must be in meters\n"
                         "const float atmosphereHeight="+QString::number(atmosphereHeight)+"; // must be in meters\n"
                         R"(
const vec3 earthCenter=vec3(0,0,-earthRadius);

const float dobsonUnit = 2.687e20; // molecules/m^2
const float PI=3.1415926535897932;
const float km=1000;
#define sqr(x) ((x)*(x))

const float sunAngularRadius=)" + toString(sunAngularRadius) + R"(;
const vec4 scatteringTextureSize=)" + toString(scatteringTextureSize) + R"(;
const vec2 irradianceTextureSize=)" + toString(glm::vec2(irradianceTexW, irradianceTexH)) + R"(;
const vec2 transmittanceTextureSize=)" + toString(glm::vec2(transmittanceTexW,transmittanceTexH)) + R"(;
const int radialIntegrationPoints=)" + toString(radialIntegrationPoints) + R"(;
const int numTransmittanceIntegrationPoints=)" + toString(numTransmittanceIntegrationPoints) + R"(;
)";
}

QString makeDensitiesFunctions()
{
    QString header;
    QString src;
    for(auto const& scatterer : scatterers)
    {
        src += "float scattererNumberDensity_"+scatterer.name+"(float altitude)\n"
               "{\n"
               +scatterer.numberDensity+
               "}\n";
        header += "float scattererNumberDensity_"+scatterer.name+"(float altitude);\n";
    }
    for(auto const& absorber : absorbers)
    {
        src += "float absorberNumberDensity_"+absorber.name+"(float altitude)\n"
               "{\n"
               +absorber.numberDensity+
               "}\n";
        header += "float absorberNumberDensity_"+absorber.name+"(float altitude);\n";
    }

    header += "vec4 scatteringCrossSection();\n"
              "float scattererDensity(float altitude);\n";

    if(densitiesHeader.isEmpty())
        densitiesHeader=header;

    return src;
}

QString makeTransmittanceComputeFunctionsSrc(glm::vec4 const& wavelengths)
{
    const QString head=1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "common-functions.h.glsl"
)";
    const QString opticalDepthFunctionTemplate=R"(
vec4 opticalDepthToAtmosphereBorder_##agentSpecies(float altitude, float cosZenithAngle, vec4 crossSection)
{
    const float integrInterval=distanceToAtmosphereBorder(cosZenithAngle, altitude);

    const float R=earthRadius;
    const float r1=R+altitude;
    const float l=integrInterval;
    const float mu=cosZenithAngle;
    /* From law of cosines: r₂²=r₁²+l²+2r₁lμ */
    const float endAltitude=-R+sqrt(sqr(r1)+sqr(l)+2*r1*l*mu);

    const float dl=integrInterval/(numTransmittanceIntegrationPoints-1);

    /* Using trapezoid rule on a uniform grid: f0/2+f1+f2+...+f(N-2)+f(N-1)/2. */
    float sum=(agent##NumberDensity_##agentSpecies(altitude)+
               agent##NumberDensity_##agentSpecies(endAltitude))/2;
    for(int n=1;n<numTransmittanceIntegrationPoints-1;++n)
    {
        const float dist=n*dl;
        const float currAlt=-R+sqrt(sqr(r1)+sqr(dist)+2*r1*dist*mu);
        sum+=agent##NumberDensity_##agentSpecies(currAlt);
    }
    return sum*dl*crossSection;
}
)";
    QString opticalDepthFunctions;
    QString computeFunction = R"(
// This assumes that ray doesn't intersect Earth
vec4 computeTransmittanceToAtmosphereBorder(float cosZenithAngle, float altitude)
{
    const vec4 depth=
)";
    for(auto const& scatterer : scatterers)
    {
        opticalDepthFunctions += QString(opticalDepthFunctionTemplate).replace("##agentSpecies",scatterer.name).replace("agent##","scatterer");
        computeFunction += "        +opticalDepthToAtmosphereBorder_"+scatterer.name+
                             "(altitude,cosZenithAngle,"+toString(scatterer.crossSection(wavelengths))+")\n";
    }
    for(auto const& absorber : absorbers)
    {
        opticalDepthFunctions += QString(opticalDepthFunctionTemplate).replace("##agentSpecies",absorber.name).replace("agent##","absorber");
        computeFunction += "        +opticalDepthToAtmosphereBorder_"+absorber.name+
                             "(altitude,cosZenithAngle,"+toString(absorber.crossSection(wavelengths))+")\n";
    }
    computeFunction += R"(      ;
    return exp(-depth);
}
)";
    constexpr char mainFunc[]=R"(
)";
    return withHeadersIncluded(head+makeDensitiesFunctions()+opticalDepthFunctions+computeFunction,
                               QString("(virtual)%1").arg(COMPUTE_TRANSMITTANCE_SHADER_FILENAME));
}

QString makeScattererDensityFunctionsSrc(glm::vec4 const& wavelengths)
{
    const QString head=1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
)";
    return withHeadersIncluded(head+makeDensitiesFunctions(),
                               QString("(virtual)%1").arg(DENSITIES_SHADER_FILENAME));
}

QString makePhaseFunctionsSrc(QString const& source)
{
    return withHeadersIncluded(1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"

vec4 phaseFunction(float dotViewSun)
{
)" + source.trimmed() + R"(
}
)", QString("(virtual)%1").arg(PHASE_FUNCTIONS_SHADER_FILENAME));
}

QString getShaderSrc(QString const& fileName)
{
    QFile file;
    bool opened=false;
    const auto appBinDir=QDir(qApp->applicationDirPath()+"/").canonicalPath();
    if(appBinDir==QDir(INSTALL_BINDIR).canonicalPath())
    {
        file.setFileName(DATA_ROOT_DIR + fileName);
        opened=file.open(QIODevice::ReadOnly);
    }
    else if(appBinDir==QDir(BUILD_BINDIR).canonicalPath())
    {
        file.setFileName(SOURCE_DIR + fileName);
        opened = file.open(QIODevice::ReadOnly);
    }

    if(!opened)
    {
        std::cerr << "Error opening shader \"" << fileName.toStdString() << "\"\n";
        throw MustQuit{};
    }
    return file.readAll();
}

std::unique_ptr<QOpenGLShader> compileShader(QOpenGLShader::ShaderType type, QString source, QString const& description)
{
    auto shader=std::make_unique<QOpenGLShader>(type);
    source=withHeadersIncluded(source, description);
    if(!shader->compileSourceCode(source))
    {
        std::cerr << "Failed to compile " << description.toStdString() << ":\n"
                  << shader->log().toStdString() << "\n";
        std::cerr << "Source of the shader:\n________________________________________________\n";
        const auto lineCount=source.count(QChar('\n'));
        QTextStream srcStream(&source);
        int lineNumber=1;
        for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine(), ++lineNumber)
        {
            QRegExp lineChanger("^\\s*#\\s*line\\s+([0-9]+)\\b.*");
            std::cerr << std::setw(std::ceil(std::log10(lineCount))) << lineNumber << " " << line.toStdString() << "\n";
            if(lineChanger.exactMatch(line))
            {
                lineNumber = lineChanger.cap(1).toInt() - 1;
                continue;
            }
        }
        std::cerr << "________________________________________________\n";
        throw MustQuit{};
    }
    if(!shader->log().isEmpty())
    {
        std::cerr << "Warnings while compiling " << description.toStdString() << ":\n"
                  << shader->log().toStdString() << "\n";
    }
    return shader;
}

std::unique_ptr<QOpenGLShader> compileShader(QOpenGLShader::ShaderType type, QString const& filename)
{
    const auto src=getShaderSrc(filename);
    return compileShader(type, src, filename);
}

QOpenGLShader& getOrCompileShader(QOpenGLShader::ShaderType type, QString const& filename)
{
    const auto it=allShaders.find(filename);
    if(it!=allShaders.end()) return *it->second;
    return *allShaders.emplace(filename, compileShader(type, filename)).first->second;
}

QString withHeadersIncluded(QString src, QString const& filename)
{
    QTextStream srcStream(&src);
    int lineNumber=1;
    int headerNumber=1;
    QString newSrc;
    for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine(), ++lineNumber)
    {
        if(!line.simplified().startsWith("#include \""))
        {
            newSrc.append(line+'\n');
            continue;
        }
        auto includePattern=QRegExp("^#include \"([^\"]+)\"$");
        if(!includePattern.exactMatch(line))
        {
            std::cerr << filename.toStdString() << ":" << lineNumber << ": syntax error in #include directive\n";
            throw MustQuit{};
        }
        const auto includeFileName=includePattern.cap(1);
        static const char headerSuffix[]=".h.glsl";
        if(!includeFileName.endsWith(headerSuffix))
        {
            std::cerr << filename.toStdString() << ":" << lineNumber << ": file to include must have suffix \""
                      << headerSuffix << "\"\n";
            throw MustQuit{};
        }
        const auto header = includeFileName==CONSTANTS_HEADER_FILENAME ? constantsHeader :
                            includeFileName==DENSITIES_HEADER_FILENAME ? densitiesHeader :
                                getShaderSrc(includeFileName);
        newSrc.append(QString("#line 1 %1 // %2\n").arg(headerNumber++).arg(includeFileName));
        newSrc.append(header);
        newSrc.append(QString("#line %1 0 // %2\n").arg(lineNumber+1).arg(filename));
    }
    return newSrc;
}

std::set<QString> getShaderFileNamesToLinkWith(QString const& filename, int recursionDepth=0)
{
    constexpr int maxRecursionDepth=50;
    if(recursionDepth>maxRecursionDepth)
    {
        std::cerr << "Include recursion depth exceeded " << maxRecursionDepth << "\n";
        throw MustQuit{};
    }
    std::set<QString> filenames;
    auto shaderSrc=getShaderSrc(filename);
    QTextStream srcStream(&shaderSrc);
    for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine())
    {
        auto includePattern=QRegExp("^#include \"([^\"]+)(\\.h\\.glsl)\"$");
        if(!includePattern.exactMatch(line))
            continue;
        const auto includeFileBaseName=includePattern.cap(1);
        if(includeFileBaseName+includePattern.cap(2) == CONSTANTS_HEADER_FILENAME) // no companion source for constants header
            continue;
        const auto shaderFileNameToLinkWith=includeFileBaseName+".frag";
        filenames.insert(shaderFileNameToLinkWith);
        if(!internalShaders.count(shaderFileNameToLinkWith) && shaderFileNameToLinkWith!=filename)
        {
            const auto extraFileNames=getShaderFileNamesToLinkWith(shaderFileNameToLinkWith, recursionDepth+1);
            filenames.insert(extraFileNames.begin(), extraFileNames.end());
        }
    }
    return filenames;
}

std::unique_ptr<QOpenGLShaderProgram> compileShaderProgram(QString const& mainSrcFileName,
                                                           const char* description, const bool useGeomShader)
{
    auto program=std::make_unique<QOpenGLShaderProgram>();

    auto shaderFileNames=getShaderFileNamesToLinkWith(mainSrcFileName);
    shaderFileNames.insert(mainSrcFileName);

    for(const auto filename : shaderFileNames)
        program->addShader(&getOrCompileShader(QOpenGLShader::Fragment, filename));

    program->addShader(&getOrCompileShader(QOpenGLShader::Vertex, "shader.vert"));
    if(useGeomShader)
        program->addShader(&getOrCompileShader(QOpenGLShader::Geometry, "shader.geom"));

    if(!program->link())
    {
        // Qt prints linking errors to stderr, so don't print them again
        std::cerr << "Failed to link " << description << "\n";
        throw MustQuit{};
    }
    return program;
}
