#!/usr/bin/env ruby

IMAGES = ["turtleegglarge.jpg","rings2.png","gamma_dalai_lama_gray.jpg","u1.jpg","waterhouse.jpg","u6.jpg","premult_test.png","gamma_test.jpg"]


version_info = `convert --version` + `./flow-proto1 --version` + `./imagew --version`

`./fetch_images.sh`

`mkdir ./compare/images`
EXEPATH = "./"
IMAGEPATH = "./source_images/"
OUTPATH ="./compare/images/"
IMAGERELPATH="images/"

def create_command(tool, image, nogamma, filter, sharpen, w)
  basename = File.basename(image)
  sharpen ||= "0"
  filter ||= "robidoux"
  infile = "#{IMAGEPATH}#{image}"
  outfile = "#{basename}_#{tool}_#{nogamma ? 'nogamma' : 'linear'}_#{filter}_s#{sharpen}_w#{w}.png"
  if tool == :imagew
    w_filter = "-filter #{filter}"
    if filter == :robidoux
      w_filter = " -filter cubic0.37821575509399867,0.31089212245300067 "
    end
    if filter == :robidouxsharp
      w_filter=" -filter cubic0.2620145123990142,0.3689927438004929 "
    end
    if filter == :ncubic
      w_filter=" -filter cubic0.37821575509399867,0.31089212245300067 -blur 0.85574108326"
    end
    if filter == :ncubicsharp
      w_filter=" -filter cubic0.2620145123990142,0.3689927438004929 -blur 0.90430390753 "
    end
    if filter == :ginseng
      return nil #Because imagew doesn't do ginseng
    end

    command = "#{EXEPATH}imagew #{nogamma ? '-nogamma' : ''} #{w_filter} #{infile} -w #{w} #{OUTPATH}#{outfile}"
  end
  if tool == :magick
    magick_filter = "-filter #{filter}"
    if filter == :ginseng
      magick_filter = " -define filter:filter=Sinc -define filter:window=Jinc -define filter:lobes=3 "
    end
    if filter == :ncubic
      magick_filter = "  -filter robidoux -define filter:blur=0.85574108326 "
    end
    if filter == :bspline
      magick_filter = "  -filter spline"
    end
    if filter == :ncubicsharp
      magick_filter = " -filter robidouxsharp -define filter:blur=0.90430390753  "
    end
    if nogamma
      command = "convert #{infile} #{magick_filter} -resize #{w} #{OUTPATH}#{outfile}"
    else
      command = "convert #{infile} -set colorspace sRGB -colorspace RGB #{magick_filter}  -resize #{w} -colorspace sRGB #{OUTPATH}#{outfile}"
    end
  end
  if tool == :flow
    outputformat = image =~ /\.png/ ? "png" : "png24"
    command = "#{EXEPATH}flow-proto1 #{nogamma ? '--incorrectgamma' : ''} --down-filter #{filter} --up-filter #{filter} --format #{outputformat} -m 100 -h 9999  --sharpen #{sharpen} -w #{w} -i #{infile} -o #{OUTPATH}#{outfile}"
  end
  {command: command, image: image, gamma: nogamma ? 'nogamma' : 'linear', filter: filter, sharpen: sharpen,
    w: w, tool: tool,
    relpath: "#{IMAGERELPATH}#{outfile}", path:  "#{OUTPATH}#{outfile}"}
end

# imageflow's box filter acts differently, and is not compared here
SIZES = [200,400,800]
FILTERS = [:triangle, :lanczos, :lanczos2, :ginseng, :ncubic, :ncubicsharp, :robidoux, :robidouxsharp,
:bspline, :hermite, :catrom, :mitchell]
GAMMAS = [true, false]
SHARPENS = [0,2,5,10]

def generate_for(tool)
  commands = []
  IMAGES.each do |img|
    SIZES.each do |width|
      FILTERS.each do |filter|
        GAMMAS.each do |gamma|
          (tool == :flow ? SHARPENS : [0]).each do |sharpen|
            commands << create_command(tool, img, gamma, filter, sharpen, width)
          end
        end
      end
    end
  end
  commands.compact
end

tools = [:flow, :magick, :imagew]

require 'thread'

flow_commands = generate_for(:flow)

commands = generate_for(:imagew) + generate_for(:magick) + flow_commands

queue = Queue.new
commands.each {|c| queue << c}

completed_work = []

Thread.abort_on_exception = true

consumers = (0..5).map do |t|
  Thread.new do
    while !queue.empty? do
      work = queue.pop
      unless File.exist? work[:path]
        work[:output] = `#{work[:command]}`
      end
      completed_work << work
    end
  end
end

consumers.each{ |c| c.join }

flow_commands.each {|c| queue << c}

consumers = (0..5).map do |t|
  Thread.new do
    while !queue.empty? do
      work = queue.pop
      compare_to_path = work[:path].gsub(/_flow_/,"_imagew_")
      next unless File.exist? compare_to_path

      dssim_path = "#{work[:path]}_dssim.txt"
      if File.exist? dssim_path
        work[:dssim] = IO.read(dssim_path)
      else
        work[:dssim] = `dssim #{work[:path]} #{compare_to_path}`
        IO.write(dssim_path, work[:dssim]) if $?.exitstatus == 0
      end
      work[:dssim_value] = work[:dssim].strip.to_f
    end
  end
end
consumers.each{ |c| c.join }

completed_work.select{|w| !w[:dssim_value].nil? && w[:dssim_value] > 0 }.sort_by{|w| w[:dssim_value]}.reverse.take(100).each do |w|
  puts "#{w[:dssim].strip}: #{w[:relpath]}\n"
end


#TODO: run dssim/compare

require 'json'

json_hash = {info: version_info, images: completed_work, image_names: IMAGES, widths: SIZES,
tools: tools, filters: FILTERS, sharpen_values: SHARPENS, gamma_values: ["nogamma", "linear"]}

IO.write("./compare/data.js", "window.data = " + JSON.pretty_generate(json_hash) + ";")

