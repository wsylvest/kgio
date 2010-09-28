# -*- encoding: binary -*-

# most tasks are in the GNUmakefile which offers better parallelism

def tags
  timefmt = '%Y-%m-%dT%H:%M:%SZ'
  @tags ||= `git tag -l`.split(/\n/).map do |tag|
    if %r{\Av[\d\.]+\z} =~ tag
      header, subject, body = `git cat-file tag #{tag}`.split(/\n\n/, 3)
      header = header.split(/\n/)
      tagger = header.grep(/\Atagger /).first
      body ||= "initial"
      {
        :time => Time.at(tagger.split(/ /)[-2].to_i).utc.strftime(timefmt),
        :tagger_name => %r{^tagger ([^<]+)}.match(tagger)[1].strip,
        :tagger_email => %r{<([^>]+)>}.match(tagger)[1].strip,
        :id => `git rev-parse refs/tags/#{tag}`.chomp!,
        :tag => tag,
        :subject => subject,
        :body => body,
      }
    end
  end.compact.sort { |a,b| b[:time] <=> a[:time] }
end

cgit_url = "http://git.bogomips.org/cgit/kgio.git"
git_url = ENV['GIT_URL'] || 'git://git.bogomips.org/kgio.git'
web_url = "http://unicorn.bogomips.org/kgio"

desc 'prints news as an Atom feed'
task :news_atom do
  require 'nokogiri'
  new_tags = tags[0,10]
  puts(Nokogiri::XML::Builder.new do
    feed :xmlns => "http://www.w3.org/2005/Atom" do
      id! "#{web_url}NEWS.atom.xml"
      title "kgio news"
      subtitle "socket methods using MSG_DONTWAIT and more"
      link! :rel => "alternate", :type => "text/html",
            :href => "#{web_url}NEWS.html"
      updated(new_tags.empty? ? "1970-01-01T00:00:00Z" : new_tags.first[:time])
      new_tags.each do |tag|
        entry do
          title tag[:subject]
          updated tag[:time]
          published tag[:time]
          author {
            name tag[:tagger_name]
            email tag[:tagger_email]
          }
          url = "#{cgit_url}/tag/?id=#{tag[:tag]}"
          link! :rel => "alternate", :type => "text/html", :href =>url
          id! url
          message_only = tag[:body].split(/\n.+\(\d+\):\n {6}/s).first.strip
          content({:type =>:text}, message_only)
          content(:type =>:xhtml) { pre tag[:body] }
        end
      end
    end
  end.to_xml)
end

desc 'prints RDoc-formatted news'
task :news_rdoc do
  tags.each do |tag|
    time = tag[:time].tr!('T', ' ').gsub!(/:\d\dZ/, ' UTC')
    puts "=== #{tag[:tag].sub(/^v/, '')} / #{time}"
    puts ""

    body = tag[:body]
    puts tag[:body].gsub(/^/sm, "  ").gsub(/[ \t]+$/sm, "")
    puts ""
  end
end

desc "print release changelog for Rubyforge"
task :release_changes do
  version = ENV['VERSION'] or abort "VERSION= needed"
  version = "v#{version}"
  vtags = tags.map { |tag| tag[:tag] =~ /\Av/ and tag[:tag] }.sort
  prev = vtags[vtags.index(version) - 1]
  if prev
    system('git', 'diff', '--stat', prev, version) or abort $?
    puts ""
    system('git', 'log', "#{prev}..#{version}") or abort $?
  else
    system('git', 'log', version) or abort $?
  end
end

desc "print release notes for Rubyforge"
task :release_notes do
  spec = Gem::Specification.load('kgio.gemspec')
  puts spec.description.strip
  puts ""
  puts "* #{spec.homepage}"
  puts "* #{spec.email}"
  puts "* #{git_url}"

  _, _, body = `git cat-file tag v#{spec.version}`.split(/\n\n/, 3)
  print "\nChanges:\n\n"
  puts body
end

desc "post news article to rubyforge"
task :publish_news do
  require 'rubyforge'
  spec = Gem::Specification.load('kgio.gemspec')
  tmp = Tempfile.new('rf-news')
  _, subject, body = `git cat-file tag v#{spec.version}`.split(/\n\n/, 3)
  tmp.puts subject
  tmp.puts
  tmp.puts spec.description.strip
  tmp.puts ""
  tmp.puts "* #{spec.homepage}"
  tmp.puts "* #{spec.email}"
  tmp.puts "* #{git_url}"
  tmp.print "\nChanges:\n\n"
  tmp.puts body
  tmp.flush
  system(ENV["VISUAL"], tmp.path) or abort "#{ENV["VISUAL"]} failed: #$?"
  msg = File.readlines(tmp.path)
  subject = msg.shift
  blank = msg.shift
  blank == "\n" or abort "no newline after subject!"
  subject.strip!
  body = msg.join("").strip!

  rf = RubyForge.new.configure
  rf.login
  rf.post_news('rainbows', subject, body)
end

desc "post to RAA"
task :raa_update do
  require 'net/http'
  require 'net/netrc'
  rc = Net::Netrc.locate('kgio-raa') or abort "~/.netrc not found"
  password = rc.password

  s = Gem::Specification.load('kgio.gemspec')
  desc = [ s.description.strip ]
  desc << ""
  desc << "* #{s.email}"
  desc << "* #{git_url}"
  desc << "* #{cgit_url}"
  desc = desc.join("\n")
  uri = URI.parse('http://raa.ruby-lang.org/regist.rhtml')
  form = {
    :name => s.name,
    :short_description => s.summary,
    :version => s.version.to_s,
    :status => 'experimental',
    :owner => s.authors.first,
    :email => s.email,
    :category_major => 'Library',
    :category_minor => 'System',
    :url => s.homepage,
    :download => 'http://rubyforge.org/frs/?group_id=8977',
    :license => "Ruby's",
    :description_style => 'Plain',
    :description => desc,
    :pass => password,
    :submit => 'Update',
  }
  res = Net::HTTP.post_form(uri, form)
  p res
  puts res.body
end
