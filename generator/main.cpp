#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <random>
#include <map>
#include <set>

#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QApplication>
#include <QRegExp>
#include <QImage>
#include <QFile>

#include "config.h"
#include "data.hpp"
#include "util.hpp"
#include "glinit.hpp"
#include "cmdline.hpp"
#include "shaders.hpp"
#include "../cie-xyzw-functions.hpp"

QOpenGLFunctions_3_3_Core gl;

void saveIrradiance(const int scatteringOrder, const int texIndex)
{
    if(!dbgSaveGroundIrradiance) return;
    saveTexture(GL_TEXTURE_2D,textures[TEX_DELTA_IRRADIANCE],"irradiance texture",
                textureOutputDir+"/irradiance-delta-order"+std::to_string(scatteringOrder-1)+"-wlset"+std::to_string(texIndex)+".f32",
                {float(irradianceTexW), float(irradianceTexH)});

    saveTexture(GL_TEXTURE_2D,textures[TEX_IRRADIANCE],"irradiance texture",
                textureOutputDir+"/irradiance-accum-order"+std::to_string(scatteringOrder-1)+"-wlset"+std::to_string(texIndex)+".f32",
                {float(irradianceTexW), float(irradianceTexH)});
}

void saveScatteringDensity(const int scatteringOrder, const int texIndex)
{
    if(!dbgSaveScatDensity) return;
    saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING_DENSITY],
                "order "+std::to_string(scatteringOrder)+" scattering density",
                textureOutputDir+"/scattering-density"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                {scatteringTextureSize[0], scatteringTextureSize[1], scatteringTextureSize[2], scatteringTextureSize[3]});
}

void render3DTexLayers(QOpenGLShaderProgram& program, const std::string_view whatIsBeingDone)
{
    std::cerr << indentOutput() << whatIsBeingDone << "... ";
    for(unsigned layer=0; layer<scatTexDepth(); ++layer)
    {
        std::ostringstream ss;
        ss << layer << " of " << scatTexDepth() << " layers done";
        std::cerr << ss.str();

        program.setUniformValue("layer",layer);
        renderQuad();
        gl.glFinish();

        // Clear previous status and reset cursor position
        const auto statusWidth=ss.tellp();
        std::cerr << std::string(statusWidth, '\b') << std::string(statusWidth, ' ')
                  << std::string(statusWidth, '\b');
    }
    std::cerr << "done\n";
}

void computeTransmittance(const int texIndex)
{
    const auto program=compileShaderProgram("compute-transmittance.frag", "transmittance computation shader program");

    std::cerr << indentOutput() << "Computing transmittance... ";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_TRANSMITTANCE]);
    assert(fbos[FBO_TRANSMITTANCE]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_TRANSMITTANCE],0);
    checkFramebufferStatus("framebuffer for transmittance texture");

    program->bind();
    gl.glViewport(0, 0, transmittanceTexW, transmittanceTexH);
    renderQuad();

    gl.glFinish();
    std::cerr << "done\n";

    saveTexture(GL_TEXTURE_2D,textures[TEX_TRANSMITTANCE],"transmittance texture",
                textureOutputDir+"/transmittance-wlset"+std::to_string(texIndex)+".f32",
                {float(transmittanceTexW), float(transmittanceTexH)});

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeDirectGroundIrradiance(const int texIndex)
{
    const auto program=compileShaderProgram("compute-direct-irradiance.frag", "direct ground irradiance computation shader program");

    std::cerr << indentOutput() << "Computing direct ground irradiance... ";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_IRRADIANCE],0);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,textures[TEX_IRRADIANCE],0);
    checkFramebufferStatus("framebuffer for irradiance texture");
    setDrawBuffers({GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1});

    program->bind();

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");
    program->setUniformValue("solarIrradianceAtTOA",QVec(solarIrradianceAtTOA[texIndex]));

    gl.glViewport(0, 0, irradianceTexW, irradianceTexH);
    renderQuad();

    gl.glFinish();
    std::cerr << "done\n";

    saveIrradiance(1,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeSingleScattering(const int texIndex, ScattererDescription const& scatterer)
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_DELTA_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_DELTA_SCATTERING],0);
    checkFramebufferStatus("framebuffer for first scattering");

    gl.glViewport(0, 0, scatTexWidth(), scatTexHeight());

    const auto src=makeScattererDensityFunctionsSrc()+
                    "float scattererDensity(float alt) { return scattererNumberDensity_"+scatterer.name+"(alt); }\n"+
                    "vec4 scatteringCrossSection() { return "+toString(scatterer.crossSection(allWavelengths[texIndex]))+"; }\n";
    allShaders.erase(DENSITIES_SHADER_FILENAME);
    virtualSourceFiles[DENSITIES_SHADER_FILENAME]=src;
    const auto program=compileShaderProgram("compute-single-scattering.frag",
                                            "single scattering computation shader program",
                                            true);
    program->bind();
    const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks
    program->setUniformValue("solarIrradianceAtTOA",QVec(solarIrradianceAtTOA[texIndex]));
    program->setUniformValue("altitudeMin", altitudeMin);
    program->setUniformValue("altitudeMax", altitudeMax);

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");

    render3DTexLayers(*program, "Computing single scattering layers");

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeIndirectIrradianceOrder1(int texIndex, int scattererIndex);
void computeScatteringDensityOrder2(const int texIndex)
{
    constexpr int scatteringOrder=2;

    allShaders.erase(DENSITIES_SHADER_FILENAME);
    virtualSourceFiles[DENSITIES_SHADER_FILENAME]=makeScattererDensityFunctionsSrc();
    std::unique_ptr<QOpenGLShaderProgram> program;
    {
        // Make a stub for current phase function. It's not used for ground radiance, but we need it to avoid linking errors.
        const auto src=makePhaseFunctionsSrc()+
            "vec4 currentPhaseFunction(float dotViewSun) { return vec4(3.4028235e38); }\n";
        allShaders.erase(PHASE_FUNCTIONS_SHADER_FILENAME);
        virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=src;

        // Doing replacements instead of using uniforms is meant to
        //  1) Improve performance by statically avoiding branching
        //  2) Ease debugging by clearing the list of really-used uniforms (this can be printed by dumpActiveUniforms())
        allShaders.erase(COMPUTE_SCATTERING_DENSITY_FILENAME);
        virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "true")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
        // recompile the program
        program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                     "scattering density computation shader program", true);
    }

    gl.glViewport(0, 0, scatTexWidth(), scatTexHeight());

    program->bind();

    const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks
    program->setUniformValue("altitudeMin", altitudeMin);
    program->setUniformValue("altitudeMax", altitudeMax);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_SCATTERING_DENSITY],0);
    checkFramebufferStatus("framebuffer for scattering density");

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE   ,0,"transmittanceTexture");
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_DELTA_IRRADIANCE,1,"irradianceTexture");

    render3DTexLayers(*program, "Computing scattering density layers for radiation from the ground");

    if(dbgSaveScatDensityOrder2FromGround)
    {
        saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING_DENSITY],
                    "order 2 scattering density from ground texture",
                    textureOutputDir+"/scattering-density2-from-ground-wlset"+std::to_string(texIndex)+".f32",
                    {scatteringTextureSize[0], scatteringTextureSize[1], scatteringTextureSize[2], scatteringTextureSize[3]});
    }

    gl.glBlendFunc(GL_ONE, GL_ONE);
    for(unsigned scattererIndex=0; scattererIndex<scatterers.size(); ++scattererIndex)
    {
        const auto& scatterer=scatterers[scattererIndex];
        std::cerr << indentOutput() << "Processing scatterer \""+scatterer.name.toStdString()+"\":\n";
        OutputIndentIncrease incr;

        computeSingleScattering(texIndex, scatterer);
        gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);

        {
            const auto src=makePhaseFunctionsSrc()+
                "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";
            allShaders.erase(PHASE_FUNCTIONS_SHADER_FILENAME);
            virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=src;

            allShaders.erase(COMPUTE_SCATTERING_DENSITY_FILENAME);
            virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "false")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
            // recompile the program
            program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                                        "scattering density computation shader program", true);
        }
        program->bind();

        setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,1,"firstScatteringTexture");

        program->setUniformValue("altitudeMin", altitudeMin);
        program->setUniformValue("altitudeMax", altitudeMax);

        gl.glEnable(GL_BLEND);
        render3DTexLayers(*program, "Computing scattering density layers");

        computeIndirectIrradianceOrder1(texIndex, scattererIndex);
    }
    gl.glDisable(GL_BLEND);
    saveScatteringDensity(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeScatteringDensity(const int scatteringOrder, const int texIndex)
{
    assert(scatteringOrder>2);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_SCATTERING_DENSITY],0);

    allShaders.erase(COMPUTE_SCATTERING_DENSITY_FILENAME);
    virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "false")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
    // recompile the program
    const std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                                                             "scattering density computation shader program",
                                                                             true);
    program->bind();

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE   ,0,"transmittanceTexture");
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_DELTA_IRRADIANCE,1,"irradianceTexture");
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,2,"multipleScatteringTexture");

    const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks
    program->setUniformValue("altitudeMin", altitudeMin);
    program->setUniformValue("altitudeMax", altitudeMax);

    render3DTexLayers(*program, "Computing scattering density layers");
    saveScatteringDensity(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeIndirectIrradianceOrder1(const int texIndex, const int scattererIndex)
{
    constexpr int scatteringOrder=2;

    const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks

    gl.glViewport(0, 0, irradianceTexW, irradianceTexH);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    if(scattererIndex==0)
        gl.glDisablei(GL_BLEND, 0); // First scatterer overwrites delta-irradiance-texture
    else
        gl.glEnablei(GL_BLEND, 0); // Second and subsequent scatterers blend into delta-irradiance-texture
    gl.glEnablei(GL_BLEND, 1); // Total irradiance is always accumulated

    const auto& scatterer=scatterers[scattererIndex];

    allShaders.erase(PHASE_FUNCTIONS_SHADER_FILENAME);
    virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
        "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";

    allShaders.erase(COMPUTE_INDIRECT_IRRADIANCE_FILENAME);
    virtualSourceFiles[COMPUTE_INDIRECT_IRRADIANCE_FILENAME]=getShaderSrc(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,IgnoreCache{})
                                                .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder-1));
    std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,
                                                                       "indirect irradiance computation shader program");
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"firstScatteringTexture");
    program->setUniformValue("altitudeMin", altitudeMin);
    program->setUniformValue("altitudeMax", altitudeMax);

    std::cerr << indentOutput() << "Computing indirect irradiance... ";
    renderQuad();
    gl.glFinish();
    std::cerr << "done\n";

    gl.glDisable(GL_BLEND);
    saveIrradiance(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeIndirectIrradiance(const int scatteringOrder, const int texIndex)
{
    assert(scatteringOrder>2);
    const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks

    gl.glViewport(0, 0, irradianceTexW, irradianceTexH);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    gl.glDisablei(GL_BLEND, 0); // Overwrite delta-irradiance-texture
    gl.glEnablei(GL_BLEND, 1); // Accumulate total irradiance

    allShaders.erase(COMPUTE_INDIRECT_IRRADIANCE_FILENAME);
    virtualSourceFiles[COMPUTE_INDIRECT_IRRADIANCE_FILENAME]=getShaderSrc(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,IgnoreCache{})
                                                .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder-1));
    std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,
                                                                       "indirect irradiance computation shader program");
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"multipleScatteringTexture");
    program->setUniformValue("altitudeMin", altitudeMin);
    program->setUniformValue("altitudeMax", altitudeMax);

    std::cerr << indentOutput() << "Computing indirect irradiance... ";
    renderQuad();
    gl.glFinish();
    std::cerr << "done\n";

    gl.glDisable(GL_BLEND);
    saveIrradiance(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void accumulateMultipleScattering(const int scatteringOrder, const int texIndex)
{
    // We didn't render to the accumulating texture when computing delta scattering to avoid holding
    // more than two 4D textures in VRAM at once.
    // Now it's time to do this by only holding the accumulator and delta scattering texture in VRAM.
    gl.glActiveTexture(GL_TEXTURE0);
    if(scatteringOrder>2 || (texIndex>0 && !saveResultAsRadiance))
        gl.glEnable(GL_BLEND);
    else
        gl.glDisable(GL_BLEND);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_MULTIPLE_SCATTERING],0);
    checkFramebufferStatus("framebuffer for accumulation of multiple scattering data");

    const auto program=compileShaderProgram("copy-scattering-texture.frag",
                                            "scattering texture copy-blend shader program",
                                            true);
    program->bind();
    if(!saveResultAsRadiance)
    {
        using glm::mat4;
        const auto diag=[](GLfloat x, GLfloat y, GLfloat z, GLfloat w) { return mat4(x,0,0,0,
                                                                                     0,y,0,0,
                                                                                     0,0,z,0,
                                                                                     0,0,0,w); };
        const int wlCount = 4*allWavelengths.size();
        // Weights for the trapezoidal quadrature rule
        const mat4 weights = wlCount==4            ? diag(0.5,1,1,0.5) :
                             texIndex==0           ? diag(0.5,1,1,1  ) :
                             texIndex+1==wlCount/4 ? diag(  1,1,1,0.5) :
                                                     diag(  1,1,1,1);
        const mat4 dlambda = weights * abs(allWavelengths.back()[3]-allWavelengths.front()[0]) / (wlCount-1.f);
        // Ref: Rapport BIPM-2019/05. Principles Governing Photometry, 2nd edition. Sections 6.2, 6.3.
        const mat4 maxLuminousEfficacy=diag(683.002,683.002,683.002,1700.13); // lm/W
        const mat4 radianceToLuminance=maxLuminousEfficacy * mat4(wavelengthToXYZW(allWavelengths[texIndex][0]),
                                                                  wavelengthToXYZW(allWavelengths[texIndex][1]),
                                                                  wavelengthToXYZW(allWavelengths[texIndex][2]),
                                                                  wavelengthToXYZW(allWavelengths[texIndex][3])) * dlambda;
        // radianceToLuminance wouldn't need to be transposed, if QMatrix4x4 was column-major as glm::mat4.
        // But alas, it's not, so we do need to transpose.
        program->setUniformValue("radianceToLuminance", QMatrix4x4(&radianceToLuminance[0][0]).transposed());
    }
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"tex");
    render3DTexLayers(*program, "Blending multiple scattering layers into accumulator texture");
    gl.glDisable(GL_BLEND);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);

    if(dbgSaveAccumScattering)
    {
        saveTexture(GL_TEXTURE_3D,textures[TEX_MULTIPLE_SCATTERING],
                    "multiple scattering accumulator texture",
                    textureOutputDir+"/multiple-scattering-to-order"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                    {scatteringTextureSize[0], scatteringTextureSize[1], scatteringTextureSize[2], scatteringTextureSize[3]});
    }
    if(scatteringOrder==scatteringOrdersToCompute && (texIndex+1u==allWavelengths.size() || saveResultAsRadiance))
    {
        const auto filename = saveResultAsRadiance ?
            textureOutputDir+"/multiple-scattering-wlset"+std::to_string(texIndex)+".f32" :
            textureOutputDir+"/multiple-scattering-xyzw.f32";
        saveTexture(GL_TEXTURE_3D,textures[TEX_MULTIPLE_SCATTERING],
                    "multiple scattering accumulator texture", filename,
                    {scatteringTextureSize[0], scatteringTextureSize[1], scatteringTextureSize[2], scatteringTextureSize[3]});
    }
}

void computeMultipleScatteringFromDensity(const int scatteringOrder, const int texIndex)
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_DELTA_SCATTERING],0);
    checkFramebufferStatus("framebuffer for delta multiple scattering");

    gl.glViewport(0, 0, scatTexWidth(), scatTexHeight());

    {
        const auto program=compileShaderProgram("compute-multiple-scattering.frag",
                                                "multiple scattering computation shader program",
                                                true);
        program->bind();

        const GLfloat altitudeMin=0, altitudeMax=atmosphereHeight; // TODO: implement splitting of calculations over altitude blocks
        program->setUniformValue("altitudeMin", altitudeMin);
        program->setUniformValue("altitudeMax", altitudeMax);

        setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");
        setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING_DENSITY,1,"scatteringDensityTexture");

        render3DTexLayers(*program, "Computing multiple scattering layers");

        if(dbgSaveDeltaScattering)
        {
            saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING],
                        "delta scattering texture",
                        textureOutputDir+"/delta-scattering-order"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                        {scatteringTextureSize[0], scatteringTextureSize[1], scatteringTextureSize[2], scatteringTextureSize[3]});
        }
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
    accumulateMultipleScattering(scatteringOrder, texIndex);
}

void computeMultipleScattering(const int texIndex)
{
    // Due to interleaving of calculations of first scattering for each scatterer with the
    // second-order scattering density and irradiance we have to do this iteration separately.
    {
        std::cerr << indentOutput() << "Working on scattering orders 1 and 2:\n";
        OutputIndentIncrease incr;

        computeScatteringDensityOrder2(texIndex);
        computeMultipleScatteringFromDensity(2,texIndex);
    }
    for(int scatteringOrder=3; scatteringOrder<=scatteringOrdersToCompute; ++scatteringOrder)
    {
        std::cerr << indentOutput() << "Working on scattering order " << scatteringOrder << ":\n";
        OutputIndentIncrease incr;

        computeScatteringDensity(scatteringOrder,texIndex);
        computeIndirectIrradiance(scatteringOrder,texIndex);
        computeMultipleScatteringFromDensity(scatteringOrder,texIndex);
    }
}

int main(int argc, char** argv)
{
    qInstallMessageHandler(qtMessageHandler);
    QApplication app(argc, argv);
    app.setApplicationName("Atmosphere textures generator");
    app.setApplicationVersion(APP_VERSION);

    try
    {
        handleCmdLine();

        QSurfaceFormat format;
        format.setMajorVersion(3);
        format.setMinorVersion(3);
        format.setProfile(QSurfaceFormat::CoreProfile);

        QOpenGLContext context;
        context.setFormat(format);
        context.create();
        if(!context.isValid())
        {
            std::cerr << "Failed to create OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " context\n";
            return 1;
        }

        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if(!surface.isValid())
        {
            std::cerr << "Failed to create OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " offscreen surface\n";
            return 1;
        }

        context.makeCurrent(&surface);

        if(!gl.initializeOpenGLFunctions())
        {
            std::cerr << "Failed to initialize OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " functions\n";
            return 1;
        }

        init();
        for(unsigned texIndex=0;texIndex<allWavelengths.size();++texIndex)
        {
            std::cerr << "Working on wavelengths " << allWavelengths[texIndex][0] << ", "
                                                   << allWavelengths[texIndex][1] << ", "
                                                   << allWavelengths[texIndex][2] << ", "
                                                   << allWavelengths[texIndex][3] << " nm"
                         " (set " << texIndex+1 << " of " << allWavelengths.size() << "):\n";
            OutputIndentIncrease incr;

            allShaders.clear();
            initConstHeader(allWavelengths[texIndex]);
            virtualSourceFiles[COMPUTE_TRANSMITTANCE_SHADER_FILENAME]=
                makeTransmittanceComputeFunctionsSrc(allWavelengths[texIndex]);
            virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc();
            virtualSourceFiles[TOTAL_SCATTERING_COEFFICIENT_SHADER_FILENAME]=makeTotalScatteringCoefSrc();

            {
                std::cerr << indentOutput() << "Computing parts of scattering order 1:\n";
                OutputIndentIncrease incr;

                computeTransmittance(texIndex);
                // We'll use ground irradiance to take into account the contribution of light scattered by the ground to the
                // sky color. Irradiance will also be needed when we want to draw the ground itself.
                computeDirectGroundIrradiance(texIndex);
            }

            computeMultipleScattering(texIndex);
        }
    }
    catch(MustQuit&)
    {
        return 1;
    }
    catch(std::exception const& ex)
    {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 111;
    }
}